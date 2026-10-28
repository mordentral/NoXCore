// The input configuration window (input_config_window.hpp): one row per control with its two keys,
// a cell clicked waits for a key or button, and Back writes the bindings back.
#include "input_config_window.hpp"

#include "input.hpp"

#include "noxcore/io/file.hpp"

#include <algorithm>
#include <array>

namespace noxsdl {

namespace {

// InputCfg.wnd
constexpr std::int32_t kList = 910, kTitles = 911, kFirst = 912, kSecond = 913;
constexpr std::int32_t kReset = 931, kBack = 932, kDefaults = 933;
constexpr std::int32_t kPickupLeft = 971; ///< 971-973: Left, Right, Middle ('s order)
constexpr std::int32_t kPopup = 980, kPopupText = 981;
constexpr std::array<const char*, 3> kPickupNames{"Left", "Right", "Middle"};

} // namespace

InputConfigWindow::InputConfigWindow(gui::Resources res, std::filesystem::path gameDir, Callbacks callbacks)
    : gui_(std::move(res)), gameDir_(std::move(gameDir)), callbacks_(std::move(callbacks))
{
}

// A config file in the game folder (any case), parsed; nullopt if it's missing or unreadable.
std::optional<nox::config::Config> InputConfigWindow::readFile(const char* name) const
{
    try {
        const auto path = nox::findNoCase(gameDir_, name);
        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }
        const auto bytes = nox::readFileBytes(path);
        return nox::config::parseConfig(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    } catch (const std::exception& ex) {
        SDL_Log("input config: %s: %s", name, ex.what());
        return std::nullopt;
    }
}

void InputConfigWindow::open(const nox::config::Config& config)
{
    if (open_) {
        return;
    }
    open_ = true;
    config_ = config;
    capture_.reset();
    const auto proc = [this](gui::Window&, int message, gui::Window& from) { return onMessage(message, from); };
    window_ = gui_.load(gameDir_ / "window" / "InputCfg.wnd", proc);
    // The three columns tell their parent list (910), which the window's procedure stands in for.
    if (gui::Window* list = window_->find(kList)) {
        list->proc = proc;
    }
    // The file has Back and the pickup radios off; the setup turns them on (a radio
    // for each of the mouse's buttons: three here).
    if (gui::Window* back = window_->find(kBack)) {
        back->text = gui_.text("WindowDir:Back");
        back->status |= nox::gui::status::Enabled;
    }
    for (int i = 0; i < 3; ++i) {
        if (gui::Window* radio = window_->find(kPickupLeft + i)) {
            radio->status |= nox::gui::status::Enabled;
        }
    }
    // The "press a key" box: its own window over the rest, hidden until a cell is clicked.
    if (gui::Window* popup = window_->find(kPopup)) {
        popup_ = gui_.detach(popup);
        popup_->rect.x = (640 - popup_->rect.w) / 2;
        popup_->show(false);
    }
    fill(config_);
}

void InputConfigWindow::close(bool save)
{
    if (!open_) {
        return;
    }
    if (save) {
        // a key a binding, second key then first, for each row; Esc = ToggleQuitMenu.
        nox::config::Config out = config_;
        out.bindings.clear();
        for (const Row& r : rows_) {
            for (const int k : {1, 0}) {
                if (r.keys[k]) {
                    out.bindings.push_back({{*r.keys[k]}, {r.control}});
                }
            }
        }
        if (const auto* esc = nox::config::findKey("ESC")) {
            out.bindings.push_back({{esc->code}, {"ToggleQuitMenu"}});
        }
        if (callbacks_.onSave) {
            callbacks_.onSave(out);
        }
    }
    open_ = false;
    capture_.reset();
    gui_.setModal(nullptr);
    gui_.destroyAll();
    window_ = nullptr;
    popup_ = nullptr;
    if (callbacks_.onClose) {
        callbacks_.onClose();
    }
}

std::string InputConfigWindow::keyTitle(std::optional<std::uint32_t> code) const
{
    if (!code) {
        return {};
    }
    if (const auto* k = nox::config::findKey(*code)) {
        return gui_.text(std::string(k->title));
    }
    return {};
}

void InputConfigWindow::fill(const nox::config::Config& config)
{
    // every control but ToggleQuitMenu, in the table's order, with the first keys of
    // its first two bindings (a second the same as the first shows empty).
    rows_.clear();
    for (const auto& c : nox::config::controlNames()) {
        if (c.name == "ToggleQuitMenu") {
            continue;
        }
        Row r;
        r.control = std::string(c.name);
        const auto bindings = config.bindingsFor(c.name);
        if (!bindings.empty() && !bindings[0]->keys.empty()) {
            r.keys[0] = bindings[0]->keys.front();
        }
        if (bindings.size() > 1 && !bindings[1]->keys.empty() && bindings[1]->keys.front() != r.keys[0]) {
            r.keys[1] = bindings[1]->keys.front();
        }
        rows_.push_back(std::move(r));
    }
    config_.mousePickup = config.mousePickup;
    for (int i = 0; i < 3; ++i) {
        if (gui::Window* radio = window_->find(kPickupLeft + i)) {
            const bool on = config_.mousePickup == kPickupNames[static_cast<std::size_t>(i)];
            radio->state = on ? (radio->state | gui::kPressed) : (radio->state & ~gui::kPressed);
        }
    }
    showRows();
}

void InputConfigWindow::showRows()
{
    gui::Window* titles = window_->find(kTitles);
    gui::Window* first = window_->find(kFirst);
    gui::Window* second = window_->find(kSecond);
    gui::Window* list = window_->find(kList);
    if (!titles || !first || !second) {
        return;
    }
    for (gui::Window* w : {titles, first, second, list}) {
        if (w) {
            w->lines.clear();
        }
    }
    for (const Row& r : rows_) {
        titles->lines.push_back({gui_.text("bindevent:" + r.control), titles->textColor});
        first->lines.push_back({keyTitle(r.keys[0]), first->textColor});
        second->lines.push_back({keyTitle(r.keys[1]), second->textColor});
        if (list) {
            list->lines.push_back({" ", list->textColor});
        }
    }
}

void InputConfigWindow::scroll(int by)
{
    gui::Window* titles = window_ ? window_->find(kTitles) : nullptr;
    if (!titles) {
        return;
    }
    // As many rows as show (a line and a pixel each in the column's height).
    const int shown = titles->rect.h / 17;
    const int most = std::max(0, static_cast<int>(rows_.size()) - shown);
    const int to = std::clamp(titles->scroll + by, 0, most);
    for (const std::int32_t id : {kTitles, kFirst, kSecond, kList}) {
        if (gui::Window* w = window_->find(id)) {
            w->scroll = to;
        }
    }
}

void InputConfigWindow::assign(std::uint32_t code)
{
    if (!capture_) {
        return;
    }
    // The key leaves every other cell, then goes in this one.
    for (Row& r : rows_) {
        for (auto& k : r.keys) {
            if (k == code) {
                k.reset();
            }
        }
    }
    rows_[static_cast<std::size_t>(capture_->second)].keys[capture_->first] = code;  // capture_ is (column, row)
    showRows();
    cancelCapture();
}

void InputConfigWindow::cancelCapture()
{
    capture_.reset();
    if (popup_) {
        popup_->show(false);
    }
    gui_.setModal(nullptr);
    for (const std::int32_t id : {kFirst, kSecond}) {
        if (gui::Window* w = window_ ? window_->find(id) : nullptr) {
            w->selectedLine = -1; // (0x4013 -1)
        }
    }
}

bool InputConfigWindow::onMessage(int message, gui::Window& from)
{
    if (message != gui::Clicked) {
        return false;
    }
    gui_.resources().sound(0x399); // ShellClick
    switch (from.id) {
    case kReset:
        // nox.cfg's bindings again.
        if (const auto c = readFile("nox.cfg")) {
            fill(*c);
        }
        return true;
    case kDefaults:
        if (const auto c = readFile("default.cfg")) {
            fill(*c);
        }
        return true;
    case kBack:
        deferred_.push_back([this] { close(true); });
        return true;
    case kFirst:
    case kSecond:
        if (from.selectedLine >= 0 && from.selectedLine < static_cast<int>(rows_.size())) {
            // "Press a key" with the control's title, over everything until it comes.
            capture_ = std::pair{from.id == kFirst ? 0 : 1, from.selectedLine};
            if (popup_) {
                if (gui::Window* text = popup_->find(kPopupText)) {
                    const std::string control =
                        gui_.text("bindevent:" + rows_[static_cast<std::size_t>(from.selectedLine)].control);
                    text->text = gui_.text("InputCfg.wnd:PressKey") + "\n'" + control + "'";
                }
                popup_->show(true);
                gui_.setModal(popup_);
            }
        }
        return true;
    default:
        break;
    }
    if (from.id >= kPickupLeft && from.id < kPickupLeft + 3) {
        config_.mousePickup = kPickupNames[static_cast<std::size_t>(from.id - kPickupLeft)];
        return true;
    }
    return false;
}

void InputConfigWindow::update(std::uint64_t nowMs)
{
    gui_.update(nowMs);
    gui::runQueued(deferred_);
}

void InputConfigWindow::draw(SDL_Renderer* renderer, int width, int height)
{
    if (open_) {
        gui_.draw(renderer, width, height);
    }
}

void InputConfigWindow::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!open_) {
        return;
    }
    if (capture_) {
        // the next key or button goes in the cell; Escape leaves it.
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                cancelCapture();
            } else if (const auto code = InputBindings::keyCode(event.key.scancode);
                       code && nox::config::findKey(*code)) {
                assign(*code);
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (const auto code = InputBindings::mouseCode(event.button.button)) {
                assign(*code);
            }
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.y != 0) {
            assign(event.wheel.y > 0 ? nox::config::kWheelUp : nox::config::kWheelDown);
        }
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE && !event.key.repeat) {
        close(false); // (with 1: gone without writing)
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.y != 0) {
        scroll(event.wheel.y > 0 ? -1 : 1);
        return;
    }
    gui_.handleEvent(event, width, height);
}

} // namespace noxsdl
