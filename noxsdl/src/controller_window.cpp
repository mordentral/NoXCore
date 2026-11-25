// (noxcpp) The Controller screen (controller_window.hpp).
#include "controller_window.hpp"

#include "pad/pad_glyphs.hpp"
#include "text.hpp"

#include "noxcore/strings/l10n.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace noxsdl {

namespace {

namespace st = nox::gui::status;
namespace sy = nox::gui::style;
using pad::PadButton;
using Color = nox::gui::Color;

// The windows (made here, not from a file).
constexpr std::int32_t kPanel = 7200, kPicture = 7201, kLayerBox = 7202, kTabButtons = 7203, kTabSettings = 7204;
constexpr std::int32_t kButtonsPage = 7210, kList = 7211, kSettingsPage = 7220;
constexpr std::int32_t kSlider = 7240;   ///< 7240-7242: the left and right dead zones, the run's tilt
constexpr std::int32_t kDropBox = 7254;  ///< 7254-7257: aim assist, the lock, the buttons' names, Use
constexpr std::int32_t kDropItem = 7260; ///< 7260 + 10 * drop-down + choice (to 7291)
constexpr std::int32_t kInvert = 7300, kRumble = 7301;
constexpr std::int32_t kDefaults = 7310, kReset = 7311, kBack = 7312;
constexpr std::int32_t kPopup = 7320;
// The donors of the game's look: AdVidOpt.wnd's check box and button, servopts.wnd's tab.
constexpr std::int32_t kCheckTemplate = 2040, kButtonTemplate = 2099, kTabTemplate = 10160;

// The layout (the 640 x 480 screen's units).
constexpr int kPictureX = 10, kPictureY = 46, kPictureW = 356, kPictureH = 352;
constexpr float kLabelW = 86, kRowH = 24;
constexpr int kPageX = 378, kPageY = 48, kPageW = 252, kPageH = 394;
constexpr int kListTop = 16;  ///< the list under the page's headers
constexpr int kLineStep = 17; ///< a list line's 16 and a pixel
constexpr int kCell0 = 140, kCell1 = 196, kCellW = 54;
/// The settings: a row each, its label over its control.
constexpr int kRowY0 = 6, kRowStep = 34, kControlX = 6, kControlY = 13, kControlW = 238;

constexpr int kSliderCount = 3, kDropCount = 4;
constexpr std::array<std::pair<int, int>, kSliderCount> kSliderRange{{{0, 50}, {0, 50}, {30, 95}}};
constexpr std::array<int, kDropCount> kDropChoices{4, 2, 4, 2};
/// The drop-down saying which the player plays with (the input mode: not in the file).
constexpr int kUseDrop = 3;
/// The settings' rows, top down: Use, the sliders, the other drop-downs.
constexpr int sliderRow(int i)
{
    return 1 + i;
}
constexpr int dropRow(int i)
{
    return i == kUseDrop ? 0 : 1 + kSliderCount + i;
}
constexpr int kRowCount = kSliderCount + kDropCount;

const Color kGold{240, 180, 42};
const Color kDim{150, 128, 96};
const Color kLight{255, 232, 150};
const Color kBrown{150, 110, 40};

SDL_FColor fcolor(std::uint8_t r, std::uint8_t g, std::uint8_t b, float a = 1)
{
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
}
SDL_Color scolor(const Color& c)
{
    return {c.r, c.g, c.b, 255};
}

/// A plain window of a kind, no pictures.
nox::gui::WindowDef plainDef(std::int32_t id, nox::gui::WindowType type, const char* typeName, int x, int y, int w, int h)
{
    nox::gui::WindowDef d;
    d.id = id;
    d.type = type;
    d.typeName = typeName;
    d.x = x;
    d.y = y;
    d.width = w;
    d.height = h;
    d.status = st::Enabled | st::NoFocus;
    d.font = "default";
    const Color none{0, 0, 0, true};
    d.enabledColor = d.disabledColor = d.hiliteColor = d.selectedColor = d.backgroundColor = none;
    d.textColor = kGold;
    return d;
}

/// The drop-downs' and boxes' button, in Options' drop-down colours.
nox::gui::WindowDef boxDef(std::int32_t id, int x, int y, int w, int h)
{
    nox::gui::WindowDef d = plainDef(id, nox::gui::WindowType::PushButton, "PUSHBUTTON", x, y, w, h);
    d.style = sy::PushButton | sy::MouseTrack;
    d.enabledColor = {150, 110, 40};
    d.hiliteColor = {240, 180, 42};
    d.backgroundColor = {16, 12, 8};
    d.selectedColor = {70, 52, 20};
    d.disabledColor = {24, 24, 24};
    return d;
}

/// A UTF-8 string less its last character.
void dropLastChar(std::string& s)
{
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) {
            break;
        }
    }
}

/// Text wrapped to `w` (layout units), at most `maxLines` lines (the last cut short with "..."),
/// its first line's top at (x, y); the small font. Returns the lines drawn.
int fittedText(gui::Painter& p, const std::string& text, float x, float y, float w, int maxLines, const Color& c)
{
    Font* f = p.font(false);
    if (!f || text.empty() || w <= 0) {
        return 0;
    }
    const float s = p.scale();
    const SDL_FRect o = p.toOutput(0, 0, 0, 0);
    const int size = p.pixelSize(false);
    std::vector<std::string> lines = f->wrap(text, size, w * s);
    if (static_cast<int>(lines.size()) > maxLines) {
        lines.resize(static_cast<std::size_t>(maxLines));
        std::string& last = lines.back();
        while (!last.empty() && f->measure(last + "...", size) > w * s) {
            dropLastChar(last);
        }
        last += "...";
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        f->draw(lines[i], std::round(o.x + x * s), std::round(o.y + (y + 10.0f * static_cast<float>(i)) * s), size, scolor(c));
    }
    return static_cast<int>(lines.size());
}

/// A line's width in layout units (the small font).
float textWidth(gui::Painter& p, const std::string& text)
{
    Font* f = p.font(false);
    return f && !text.empty() ? f->measure(text, p.pixelSize(false)) / p.scale() : 0;
}

/// Text centred across at `cx`.
void centredText(gui::Painter& p, const std::string& text, float cx, float y, const Color& c)
{
    fittedText(p, text, cx - textWidth(p, text) / 2, y, 620, 1, c);
}

std::string joined(const std::vector<std::string>& names)
{
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += n;
    }
    return out;
}

} // namespace

ControllerWindow::ControllerWindow(gui::Resources res, std::filesystem::path windowDir, const pad::GamepadManager& pads,
                                   Callbacks callbacks)
    : gui_(std::move(res)), windowDir_(std::move(windowDir)), pads_(pads), callbacks_(std::move(callbacks))
{
}

pad::ButtonLabel ControllerWindow::label(PadButton b) const
{
    return pads_.label(b, config_.settings.prompts);
}

// ------------------------------------------------------------------------------------ building

void ControllerWindow::build()
{
    nox::gui::WindowDef panel = plainDef(kPanel, nox::gui::WindowType::User, "USER", 0, 0, 640, 480);
    panel.style = sy::User;
    panel.status = st::Enabled | st::NoFocus | st::Above;

    // The picture (drawn here; the mouse's pointing at it is read here too).
    nox::gui::WindowDef picture = plainDef(kPicture, nox::gui::WindowType::User, "USER", kPictureX, kPictureY, kPictureW, kPictureH);
    picture.style = sy::User;
    picture.status = st::Enabled | st::NoFocus | st::NoInput;

    // The donors.
    std::optional<nox::gui::WindowDef> check, button, tab;
    try {
        const auto adv = nox::gui::loadWindowFile(windowDir_ / "advidopt.wnd");
        if (const auto* d = adv.find(kCheckTemplate)) {
            check = *d;
        }
        if (const auto* d = adv.find(kButtonTemplate)) {
            button = *d;
        }
        const auto serv = nox::gui::loadWindowFile(windowDir_ / "servopts.wnd");
        if (const auto* d = serv.find(kTabTemplate)) {
            tab = *d;
        }
    } catch (const std::exception& ex) {
        SDL_Log("controller screen: %s", ex.what());
    }
    const auto checkBox = [&](std::int32_t id, int x, int y, int w) {
        nox::gui::WindowDef d = check ? *check : plainDef(id, nox::gui::WindowType::CheckBox, "CHECKBOX", x, y, w, 15);
        d.id = id;
        d.x = x;
        d.y = y;
        d.width = w;
        d.text.clear();
        d.tooltip.clear();
        d.font = "default";
        d.children.clear();
        return d;
    };
    const auto bigButton = [&](std::int32_t id, int x, int y) {
        nox::gui::WindowDef d = button ? *button : boxDef(id, x, y, 95, 20);
        d.id = id;
        d.x = x;
        d.y = y;
        d.status = (d.status | st::Enabled) & ~st::Hidden;
        d.text.clear();
        d.tooltip.clear();
        d.font = "large";
        d.children.clear();
        return d;
    };

    panel.children.push_back(picture);
    panel.children.push_back(checkBox(kLayerBox, 16, 450, 300));
    for (const auto& [id, x] : {std::pair{kTabButtons, kPageX + 2}, std::pair{kTabSettings, kPageX + 100}}) {
        nox::gui::WindowDef t = tab ? *tab : boxDef(id, x, 24, 95, 20);
        t.id = id;
        t.x = x;
        t.y = 24;
        t.height = 20;
        t.status = st::Image | st::Enabled;
        t.group = 0x7C; // (their own group: a click turns only the other off)
        t.font = "large";
        t.text.clear();
        t.tooltip.clear();
        t.children.clear();
        panel.children.push_back(std::move(t));
    }
    panel.children.push_back(bigButton(kDefaults, 630 - 3 * 95 - 2 * 6, 452));
    panel.children.push_back(bigButton(kReset, 630 - 2 * 95 - 6, 452));
    panel.children.push_back(bigButton(kBack, 630 - 95, 452));

    // Buttons: the list of rows, under its headers.
    nox::gui::WindowDef buttonsPage = plainDef(kButtonsPage, nox::gui::WindowType::User, "USER", kPageX, kPageY, kPageW, kPageH);
    buttonsPage.style = sy::User;
    nox::gui::WindowDef list = plainDef(kList, nox::gui::WindowType::ScrollListBox, "SCROLLLISTBOX", 0, kListTop, kPageW,
                                        kPageH - kListTop);
    list.style = sy::ScrollListBox | sy::MouseTrack;
    nox::gui::ListBoxData data;
    data.lines = 128;
    data.lineHeight = kLineStep - 1;
    list.data = data;
    buttonsPage.children.push_back(std::move(list));
    panel.children.push_back(std::move(buttonsPage));

    // Settings: the sliders, the drop-downs (their choices first: drawn last, over the rows below),
    // the check boxes.
    nox::gui::WindowDef settings = plainDef(kSettingsPage, nox::gui::WindowType::User, "USER", kPageX, kPageY, kPageW, kPageH);
    settings.style = sy::User;
    settings.status |= st::Hidden;
    std::vector<nox::gui::WindowDef> choices;
    for (int i = 0; i < kSliderCount; ++i) {
        nox::gui::WindowDef s = plainDef(kSlider + i, nox::gui::WindowType::HorzSlider, "HORZSLIDER", kControlX,
                                         kRowY0 + kRowStep * sliderRow(i) + kControlY + 1, kControlW, 13);
        s.style = sy::HorzSlider | sy::MouseTrack;
        s.data = nox::gui::SliderData{kSliderRange[static_cast<std::size_t>(i)].first, kSliderRange[static_cast<std::size_t>(i)].second};
        s.enabledColor = {150, 110, 40};
        s.hiliteColor = {240, 180, 42};
        s.backgroundColor = {16, 12, 8};
        s.selectedColor = {240, 180, 42};
        s.disabledColor = {24, 24, 24};
        settings.children.push_back(std::move(s));
    }
    for (int i = 0; i < kDropCount; ++i) {
        const int y = kRowY0 + kRowStep * dropRow(i) + kControlY;
        settings.children.push_back(boxDef(kDropBox + i, kControlX, y, kControlW, 17));
        for (int k = 0; k < kDropChoices[static_cast<std::size_t>(i)]; ++k) {
            nox::gui::WindowDef item = boxDef(kDropItem + 10 * i + k, kControlX, y + 17 * (k + 1), kControlW, 17);
            item.status |= st::Hidden;
            choices.push_back(std::move(item));
        }
    }
    const int checks = kRowY0 + kRowStep * kRowCount + 4;
    settings.children.push_back(checkBox(kInvert, 6, checks, kPageW - 8));
    settings.children.push_back(checkBox(kRumble, 6, checks + 22, kPageW - 8));
    settings.children.insert(settings.children.begin(), std::make_move_iterator(choices.begin()), std::make_move_iterator(choices.end()));
    panel.children.push_back(std::move(settings));

    const auto proc = [this](gui::Window&, int message, gui::Window& from) { return onMessage(message, from); };
    panel_ = gui_.load(panel, proc);
    panel_->draw = [this](gui::Window& w, gui::Painter& p) { drawPanel(w, p); };
    if (gui::Window* w = panel_->find(kPicture)) {
        w->draw = [this](gui::Window& self, gui::Painter& p) { drawPicture(self, p); };
    }
    if (gui::Window* w = panel_->find(kButtonsPage)) {
        w->draw = [](gui::Window&, gui::Painter&) {}; // (its headers: the list draws them)
    }
    if (gui::Window* w = panel_->find(kSettingsPage)) {
        w->draw = [this](gui::Window& self, gui::Painter& p) { drawSettings(self, p); };
    }
    list_ = panel_->find(kList);
    if (list_) {
        list_->largeFont = false;
        list_->navLineClicks = false; // (A, or a click, waits for a button: not every line passed)
        list_->draw = [this](gui::Window& self, gui::Painter& p) { drawList(self, p); };
        // The pad's left / right pick the line's cell; A waits for a button for it.
        list_->padInput = [this](gui::Window& self, gui::NavInput in) {
            const int line = self.selectedLine;
            const bool row = line >= 0 && line < static_cast<int>(lines_.size()) && !lines_[static_cast<std::size_t>(line)].action.empty();
            switch (in) {
            case gui::NavInput::Left:
                if (navCell_ == 1) {
                    navCell_ = 0;
                    return true;
                }
                return false;
            case gui::NavInput::Right:
                if (navCell_ == 0 && row && pad::cellUsable(lines_[static_cast<std::size_t>(line)].action, 1)) {
                    navCell_ = 1;
                    return true;
                }
                return false;
            case gui::NavInput::Accept:
                if (row) {
                    startCapture(line, navCell_, nowMs_);
                }
                return true;
            default: return false;
            }
        };
    }
    for (const std::int32_t id : {kTabButtons, kTabSettings, kDefaults, kReset, kBack}) {
        if (gui::Window* w = panel_->find(id)) {
            w->largeFont = true;
        }
    }
    for (int i = 0; i < kDropCount; ++i) {
        if (gui::Window* box = panel_->find(kDropBox + i)) {
            box->dropArrow = true;
            box->largeFont = false;
        }
        for (int k = 0; k < kDropChoices[static_cast<std::size_t>(i)]; ++k) {
            if (gui::Window* w = panel_->find(kDropItem + 10 * i + k)) {
                w->text = dropName(i, k);
                w->largeFont = false;
            }
        }
    }
    const auto setText = [this](std::int32_t id, std::string text) {
        if (gui::Window* w = panel_->find(id)) {
            w->text = std::move(text);
        }
    };
    setText(kTabButtons, NOX_TR("controller.tab.buttons", "Buttons"));
    setText(kTabSettings, NOX_TR("controller.tab.settings", "Settings"));
    setText(kDefaults, NOX_TR("controller.button.defaults", "Defaults"));
    setText(kReset, NOX_TR("controller.button.reset", "Reset"));
    setText(kBack, NOX_TR("controller.button.back", "Back"));
    setText(kInvert, NOX_TR("controller.setting.invert", "Invert the Right Stick's Up and Down"));
    setText(kRumble, NOX_TR("controller.setting.rumble", "Rumble (when hurt and on a level up)"));

    // The "press a button" box, over everything while it waits.
    nox::gui::WindowDef popup = plainDef(kPopup, nox::gui::WindowType::User, "USER", 170, 176, 300, 112);
    popup.style = sy::User;
    popup.status = st::Enabled | st::NoFocus | st::Above | st::Border | st::Hidden;
    popup.backgroundColor = {0, 0, 0};
    popup_ = gui_.load(popup, proc);
    popup_->draw = [this](gui::Window& w, gui::Painter& p) { drawPopup(w, p); };

    picture_ = pad::layoutPicture(kPictureX, kPictureY, kPictureW, kPictureH, kLabelW, kRowH);
}

void ControllerWindow::fillLines()
{
    lines_.clear();
    bool menus = false;
    for (const std::string_view a : pad::remapRows()) {
        if (pad::menuRow(a) && !menus) {
            menus = true;
            lines_.push_back({"", NOX_TR("controller.list.menus", "In the menus")});
        }
        lines_.push_back({std::string(a), pad::actionTitle(a)});
    }
    if (list_) {
        list_->lines.assign(lines_.size(), gui::Window::Line{" ", kGold});
    }
}

void ControllerWindow::open(const pad::PadConfig& config)
{
    if (open_) {
        return;
    }
    open_ = true;
    config_ = config;
    wait_.stop();
    captureLine_ = -1;
    navCell_ = 0;
    pressed_.reset();
    notice_.clear();
    mouse_ = {-1, -1};
    build();
    fillLines();
    syncSettings();
    showPage(0);
    gui::setChecked(panel_->find(kLayerBox), showLayer_);
    if (gui::Window* w = panel_->find(kLayerBox)) {
        w->text = nox::l10n::format(NOX_TR("controller.show_layer", "Show the {button} Layer"),
                                    {{"button", pad::labelWords(label(config_.modifier().value_or(PadButton::LeftTrigger)))}});
    }
    gui_.setNavDefault(list_);
    gui_.setNavTabs([this](int step) {
        const std::array<gui::Window*, 2> tabs{panel_ ? panel_->find(kTabButtons) : nullptr, panel_ ? panel_->find(kTabSettings) : nullptr};
        return gui_.stepTabs(tabs, step);
    });
}

void ControllerWindow::close(bool save)
{
    if (!open_) {
        return;
    }
    if (save && callbacks_.onSave) {
        callbacks_.onSave(config_);
    }
    open_ = false;
    wait_.stop();
    gui_.setModal(nullptr);
    gui_.setNavTabs({});
    gui_.destroyAll();
    panel_ = list_ = popup_ = nullptr;
    if (callbacks_.onClose) {
        callbacks_.onClose();
    }
}

void ControllerWindow::apply()
{
    if (callbacks_.onApply) {
        callbacks_.onApply(config_);
    }
}

void ControllerWindow::setConfig(const pad::PadConfig& c)
{
    config_ = c;
    syncSettings();
    apply();
}

// ------------------------------------------------------------------------------------ settings

int ControllerWindow::dropValue(int which) const
{
    const auto& s = config_.settings;
    if (which == kUseDrop) {
        return callbacks_.padMode && callbacks_.padMode() ? 0 : 1;
    }
    return which == 0 ? static_cast<int>(s.aimAssist) : which == 1 ? static_cast<int>(s.lockMode) : static_cast<int>(s.prompts);
}

void ControllerWindow::setDropValue(int which, int value)
{
    auto& s = config_.settings;
    if (which == kUseDrop) {
        if (callbacks_.onPadMode) {
            callbacks_.onPadMode(value == 0); // (the input mode: the program's, not the file's)
        }
    } else if (which == 0) {
        s.aimAssist = static_cast<pad::AimAssist>(value);
    } else if (which == 1) {
        s.lockMode = static_cast<pad::LockMode>(value);
    } else {
        s.prompts = static_cast<pad::PromptStyle>(value);
    }
}

std::string ControllerWindow::dropName(int which, int value) const
{
    if (which == 0) {
        switch (value) {
        case 0: return NOX_TR("controller.aim.off", "Off");
        case 1: return NOX_TR("controller.aim.weak", "Weak");
        case 2: return NOX_TR("controller.aim.normal", "Normal");
        default: return NOX_TR("controller.aim.strong", "Strong");
        }
    }
    if (which == 1) {
        return value == 0 ? NOX_TR("controller.lock.held", "Kept While Held") : NOX_TR("controller.lock.retarget", "Picked Again");
    }
    if (which == kUseDrop) {
        return value == 0 ? NOX_TR("input_mode.controller", "Controller") : NOX_TR("input_mode.mouse", "Mouse and Keyboard");
    }
    switch (value) {
    case 0: return NOX_TR("controller.prompts.auto", "As the Controller");
    case 1: return "Xbox";
    case 2: return "PlayStation";
    default: return "Switch";
    }
}

void ControllerWindow::syncSettings()
{
    if (!panel_) {
        return;
    }
    const auto& s = config_.settings;
    const std::array<int, kSliderCount> values{static_cast<int>(std::lround(s.deadZone * 100)), static_cast<int>(std::lround(s.rightDeadZone * 100)),
                                               static_cast<int>(std::lround(s.runThreshold * 100))};
    for (int i = 0; i < kSliderCount; ++i) {
        if (gui::Window* w = panel_->find(kSlider + i)) {
            const auto [lo, hi] = kSliderRange[static_cast<std::size_t>(i)];
            w->sliderValue = std::clamp(values[static_cast<std::size_t>(i)], lo, hi);
        }
    }
    for (int i = 0; i < kDropCount; ++i) {
        if (gui::Window* w = panel_->find(kDropBox + i)) {
            w->text = dropName(i, dropValue(i));
        }
    }
    gui::setChecked(panel_->find(kInvert), s.invertY);
    gui::setChecked(panel_->find(kRumble), s.rumble);
}

void ControllerWindow::showDrop(const DropDown& d, bool open)
{
    for (int k = 0; k < d.count; ++k) {
        if (gui::Window* w = panel_->find(d.item + k)) {
            w->show(open);
            gui::setChecked(w, k == dropValue((d.box - kDropBox)));
        }
    }
    // The rows under an open list are off meanwhile (the mouse finds them before its choices).
    const gui::Window* box = panel_->find(d.box);
    if (gui::Window* page = panel_->find(kSettingsPage); page && box) {
        for (auto& c : page->children) {
            const bool choice = c->id >= kDropItem && c->id < kDropItem + 10 * kDropCount;
            if (!choice && c.get() != box && c->rect.y > box->rect.y) {
                gui::setEnabled(c.get(), !open);
            }
        }
    }
}

void ControllerWindow::shutDrops()
{
    if (!panel_) {
        return;
    }
    for (int i = 0; i < kDropCount; ++i) {
        showDrop({kDropBox + i, kDropItem + 10 * i, kDropChoices[static_cast<std::size_t>(i)]}, false);
    }
}

void ControllerWindow::showPage(int page)
{
    page_ = page;
    shutDrops();
    if (gui::Window* w = panel_->find(kButtonsPage)) {
        w->show(page == 0);
    }
    if (gui::Window* w = panel_->find(kSettingsPage)) {
        w->show(page == 1);
    }
    gui::setChecked(panel_->find(kTabButtons), page == 0);
    gui::setChecked(panel_->find(kTabSettings), page == 1);
}

// ------------------------------------------------------------------------------------ the wait

void ControllerWindow::startCapture(int line, int cell, std::uint64_t nowMs)
{
    if (line < 0 || line >= static_cast<int>(lines_.size())) {
        return;
    }
    const std::string& action = lines_[static_cast<std::size_t>(line)].action;
    if (action.empty() || !pad::cellUsable(action, cell)) {
        return;
    }
    gui_.resources().sound(0x399); // ShellClick
    captureLine_ = line;
    captureCell_ = cell;
    wait_.start(nowMs, pad::backButtons(config_));
    if (popup_) {
        popup_->show(true);
        gui_.setModal(popup_);
    }
}

void ControllerWindow::endCapture()
{
    wait_.stop();
    captureLine_ = -1;
    if (popup_) {
        popup_->show(false);
    }
    gui_.setModal(nullptr);
}

void ControllerWindow::finishCapture(PadButton b)
{
    const int line = captureLine_;
    const int cell = captureCell_;
    endCapture();
    if (line < 0 || line >= static_cast<int>(lines_.size())) {
        return;
    }
    const std::string action = lines_[static_cast<std::size_t>(line)].action;
    const pad::AssignResult r = pad::assignCell(config_, action, cell, b);
    noticeUntilMs_ = nowMs_ + 4000;
    if (!r.done) {
        notice_ = NOX_TR("controller.notice.refused", "That button can't go there.");
        return;
    }
    std::vector<std::string> moved, swapped;
    for (const auto& a : r.movedFrom) {
        (std::ranges::find(r.swapped, a) != r.swapped.end() ? swapped : moved).push_back(pad::actionTitle(a));
    }
    notice_.clear();
    if (!moved.empty()) {
        notice_ = nox::l10n::format(NOX_TR("controller.notice.moved", "Taken from {actions}."), {{"actions", joined(moved)}});
    }
    if (!swapped.empty()) {
        notice_ += (notice_.empty() ? "" : " ") +
                   nox::l10n::format(NOX_TR("controller.notice.swapped", "Swapped with {actions}."), {{"actions", joined(swapped)}});
    }
    if (gui::Window* w = panel_ ? panel_->find(kLayerBox) : nullptr) {
        w->text = nox::l10n::format(NOX_TR("controller.show_layer", "Show the {button} Layer"),
                                    {{"button", pad::labelWords(label(config_.modifier().value_or(PadButton::LeftTrigger)))}});
    }
    apply();
}

void ControllerWindow::bringRowIntoView(PadButton b)
{
    if (!list_) {
        return;
    }
    const bool layer = layerShown();
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        const std::string& a = lines_[static_cast<std::size_t>(i)].action;
        if (a.empty() || pad::menuRow(a)) {
            continue;
        }
        if (pad::cellButton(config_, a, layer ? 1 : 0) == b) {
            if (page_ != 0) {
                showPage(0);
            }
            const int shown = list_->rect.h / kLineStep;
            if (i < list_->scroll || i >= list_->scroll + shown) {
                list_->scroll = std::clamp(i - shown / 2, 0, std::max(0, static_cast<int>(lines_.size()) - shown));
            }
            list_->selectedLine = i;
            navCell_ = layer ? 1 : 0;
            return;
        }
    }
}

// ------------------------------------------------------------------------------------ input

bool ControllerWindow::onMessage(int message, gui::Window& from)
{
    if (message == gui::SliderChanged && from.id >= kSlider && from.id < kSlider + kSliderCount) {
        auto& s = config_.settings;
        const int v = from.sliderValue;
        switch (from.id - kSlider) {
        case 0: s.deadZone = static_cast<float>(v) / 100.0f; break;
        case 1: s.rightDeadZone = static_cast<float>(v) / 100.0f; break;
        default: s.runThreshold = static_cast<float>(v) / 100.0f; break;
        }
        apply();
        return true;
    }
    if (message == gui::Hover) {
        gui_.resources().sound(0x398); // ShellSelect
        return false;
    }
    if (message != gui::Clicked) {
        return false;
    }
    if (from.id == kList) {
        startCapture(from.selectedLine, navCell_, nowMs_);
        return true;
    }
    gui_.resources().sound(0x399);
    if (from.id == kTabButtons || from.id == kTabSettings) {
        showPage(from.id == kTabSettings ? 1 : 0);
        return true;
    }
    if (from.id == kLayerBox) {
        showLayer_ = gui::isChecked(from);
        return true;
    }
    for (int i = 0; i < kDropCount; ++i) {
        const DropDown d{kDropBox + i, kDropItem + 10 * i, kDropChoices[static_cast<std::size_t>(i)]};
        if (from.id == d.box) {
            const gui::Window* first = panel_->find(d.item);
            const bool opening = first && first->hidden();
            shutDrops();
            showDrop(d, opening);
            gui_.padFocus(opening ? panel_->find(d.item + dropValue(i)) : &from);
            return true;
        }
        if (from.id >= d.item && from.id < d.item + d.count) {
            setDropValue(i, from.id - d.item);
            showDrop(d, false);
            gui_.padFocus(panel_->find(d.box));
            syncSettings();
            apply();
            return true;
        }
    }
    shutDrops();
    if (from.id == kInvert || from.id == kRumble) {
        (from.id == kInvert ? config_.settings.invertY : config_.settings.rumble) = gui::isChecked(from);
        apply();
        return true;
    }
    if (from.id == kDefaults) {
        pad::PadConfig d = pad::defaultPadConfig();
        d.otherLines = config_.otherLines;
        setConfig(d);
        return true;
    }
    if (from.id == kReset) {
        setConfig(callbacks_.saved ? callbacks_.saved() : pad::defaultPadConfig());
        return true;
    }
    if (from.id == kBack) {
        deferred_.push_back([this] { close(true); });
        return true;
    }
    return false;
}

void ControllerWindow::handleEvent(const SDL_Event& event, const std::optional<pad::PadEdge>& padEdge, int width, int height)
{
    if (!open_) {
        return;
    }
    const std::uint64_t now = SDL_GetTicks();
    nowMs_ = now;
    if (wait_.active()) {
        // The next pad button goes in the cell (pad_remap.hpp's rule for the Back buttons); Escape
        // on the keyboard leaves it as it was.
        if (padEdge) {
            const auto out = padEdge->down ? wait_.press(padEdge->button, now) : wait_.release(padEdge->button, now);
            if (out.kind == pad::CaptureWait::Outcome::Bound) {
                finishCapture(out.button);
            } else if (out.kind == pad::CaptureWait::Outcome::Cancelled) {
                endCapture();
            }
        } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE && !event.key.repeat) {
            endCapture();
        }
        return;
    }
    if (padEdge && padEdge->down) {
        pressed_ = padEdge->button; // (lit on the picture a moment)
        pressedMs_ = now;
        if (page_ == 0 && !gui::isNavEvent(event)) {
            bringRowIntoView(padEdge->button);
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat) {
        // An open drop-down shuts first (the pad's Back too); else gone, kept (as Back).
        for (int i = 0; i < kDropCount; ++i) {
            if (const gui::Window* first = panel_->find(kDropItem + 10 * i); first && !first->hidden()) {
                shutDrops();
                gui_.padFocus(panel_->find(kDropBox + i));
                return;
            }
        }
        close(true);
        return;
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        mouse_ = gui_.layoutPoint(event.motion.x, event.motion.y, width, height);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        mouse_ = gui_.layoutPoint(event.button.x, event.button.y, width, height);
        const auto cell = pointedCell();
        if (cell && list_ && !list_->hidden() && gui_.hovered() == list_) {
            navCell_ = cell->second;
            if (event.button.button == SDL_BUTTON_RIGHT) {
                // A right click empties the cell.
                const std::string& a = lines_[static_cast<std::size_t>(cell->first)].action;
                if (!a.empty() && pad::clearCell(config_, a, cell->second)) {
                    apply();
                }
                return;
            }
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (const auto b = pad::pictureButtonAt(picture_, mouse_.x, mouse_.y)) {
                bringRowIntoView(*b);
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (list_ && !list_->hidden() && event.wheel.y != 0) {
            const int shown = list_->rect.h / kLineStep;
            list_->scroll = std::clamp(list_->scroll + (event.wheel.y > 0 ? -1 : 1), 0,
                                       std::max(0, static_cast<int>(lines_.size()) - shown));
            return;
        }
        break;
    default:
        break;
    }
    gui_.handleEvent(event, width, height);
}

void ControllerWindow::update(std::uint64_t nowMs)
{
    nowMs_ = nowMs;
    if (!open_) {
        return;
    }
    if (wait_.active() && wait_.tick(nowMs).kind == pad::CaptureWait::Outcome::Cancelled) {
        endCapture();
    }
    gui_.update(nowMs);
    gui::runQueued(deferred_);
}

void ControllerWindow::draw(SDL_Renderer* renderer, int width, int height)
{
    if (open_) {
        gui_.draw(renderer, width, height);
    }
}

std::string ControllerWindow::tooltip() const
{
    if (!open_ || page_ != 1 || wait_.active()) {
        return {};
    }
    // The settings' rows by where the mouse is (in the pad's mode, where its focus is).
    SDL_FPoint at = mouse_;
    if (pads_.padInUse()) {
        const gui::Window* focus = gui_.navFocus();
        if (!focus) {
            return {};
        }
        const SDL_Rect r = gui::screenRect(*focus);
        at = {static_cast<float>(r.x) + static_cast<float>(r.w) / 2, static_cast<float>(r.y) + static_cast<float>(r.h) / 2};
    }
    const float rowY = at.y - static_cast<float>(kPageY + kRowY0);
    if (at.x < kPageX || at.x > kPageX + kPageW || rowY < 0) {
        return {};
    }
    const int row = static_cast<int>(rowY) / kRowStep;
    if (row == dropRow(kUseDrop)) {
        return NOX_TR("controller.tip.use",
                      "What you play with. Controller: the mouse is ignored and\n"
                      "its pointer hidden. Mouse and Keyboard: the controller is\n"
                      "ignored. Asked at each start while a controller is connected.");
    }
    if (row == sliderRow(0) || row == sliderRow(1)) {
        return NOX_TR("controller.tip.dead_zone",
                      "How far the stick must lean before it counts.\n"
                      "Raise it if the player creeps or the aim drifts\n"
                      "with the stick let go.");
    }
    if (row == sliderRow(2)) {
        return NOX_TR("controller.tip.run",
                      "How far the left stick leans before the player runs\n"
                      "(less than that walks, as a near mouse pointer does).");
    }
    if (row == dropRow(0)) {
        return NOX_TR("controller.tip.aim_assist",
                      "How strongly attacks and spells turn to the enemy you aim\n"
                      "near. Other players get only a light pull at any setting.");
    }
    if (row == dropRow(1)) {
        return NOX_TR("controller.tip.lock",
                      "Kept While Held: the target stays while the attack or spell\n"
                      "button is held. Picked Again: the best target as you turn.");
    }
    if (row == dropRow(2)) {
        return NOX_TR("controller.tip.prompts",
                      "The names and symbols shown for the buttons, on this\n"
                      "screen and on the spell bar: as the connected controller's,\n"
                      "or always one family's.");
    }
    return {};
}

// ------------------------------------------------------------------------------------ pointing

std::optional<std::pair<int, int>> ControllerWindow::pointedCell() const
{
    if (list_ && !list_->hidden()) {
        const SDL_Point at = list_->screenPos();
        if (mouse_.x >= at.x && mouse_.x < at.x + list_->rect.w && mouse_.y >= at.y && mouse_.y < at.y + list_->rect.h &&
            !pads_.padInUse()) {
            const int line = list_->scroll + static_cast<int>(mouse_.y - static_cast<float>(at.y)) / kLineStep;
            if (line >= 0 && line < static_cast<int>(lines_.size())) {
                const int cell = mouse_.x >= at.x + kCell1 ? 1 : 0;
                const std::string& a = lines_[static_cast<std::size_t>(line)].action;
                return std::pair{line, !a.empty() && pad::cellUsable(a, cell) ? cell : 0};
            }
            return std::nullopt;
        }
        if (pads_.padInUse() && gui_.navFocus() == list_ && list_->selectedLine >= 0 &&
            list_->selectedLine < static_cast<int>(lines_.size())) {
            return std::pair{list_->selectedLine, navCell_};
        }
    }
    return std::nullopt;
}

bool ControllerWindow::layerShown() const
{
    if (showLayer_) {
        return true;
    }
    if (const auto mod = config_.modifier(); mod && pads_.isDown(*mod)) {
        return true;
    }
    if (const auto cell = pointedCell()) {
        const std::string& a = lines_[static_cast<std::size_t>(cell->first)].action;
        return cell->second == 1 && !a.empty() && !pad::menuRow(a);
    }
    return false;
}

std::optional<PadButton> ControllerWindow::litButton() const
{
    if (pressed_ && (pads_.isDown(*pressed_) || nowMs_ - pressedMs_ < 700)) {
        return pressed_;
    }
    if (!pads_.padInUse()) {
        if (const auto b = pad::pictureButtonAt(picture_, mouse_.x, mouse_.y)) {
            return b;
        }
    }
    if (const auto cell = pointedCell()) {
        const std::string& a = lines_[static_cast<std::size_t>(cell->first)].action;
        if (!a.empty()) {
            return pad::cellButton(config_, a, cell->second);
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------ drawing

void ControllerWindow::drawPanel(gui::Window&, gui::Painter& p)
{
    p.fillAlpha(0, 0, 640, 480, {10, 7, 5, 250});
    p.outline(3, 3, 634, 474, kBrown);
    p.outline(5, 5, 630, 470, Color{70, 52, 20});
    p.fill(kPageX - 6, 44, 1, 400, Color{70, 52, 20});
    p.text(NOX_TR("controller.title", "Controller"), 16, 8, true, kGold);
    const std::string name = pads_.connected() ? pads_.activeName() : NOX_TR("controller.none", "No controller connected: the buttons are named by place.");
    fittedText(p, name, 16, 28, kPageX - 30, 1, kDim);
    if (!notice_.empty() && nowMs_ < noticeUntilMs_) {
        fittedText(p, notice_, 16, 430, kPageX - 30, 1, kLight);
    }
    // The list's headers.
    if (page_ == 0) {
        const int y = kPageY + 3;
        p.text(NOX_TR("controller.header.action", "Action"), kPageX + 4, y, false, kDim);
        p.text(NOX_TR("controller.header.button", "Button"), kPageX + kCell0 + 4, y, false, kDim);
        const std::string mod = pad::labelWords(label(config_.modifier().value_or(PadButton::LeftTrigger)));
        fittedText(p, mod + " +", kPageX + kCell1 + 4, static_cast<float>(y), kCellW - 4, 1, kDim);
    }
}

void ControllerWindow::drawPicture(gui::Window&, gui::Painter& p)
{
    SDL_Renderer* r = p.renderer();
    Font* font = p.font(false);
    const float s = p.scale();
    const SDL_FRect o = p.toOutput(0, 0, 0, 0);
    const auto out = [&](pad::PicturePoint q) { return SDL_FPoint{o.x + q.x * s, o.y + q.y * s}; };
    const pad::PictureLayout& L = picture_;
    const float u = L.padW * s; // (a pad unit in output pixels)
    const auto at = [&](float ux, float uy) { return out(L.toBox({ux, uy})); };
    const bool layer = layerShown();
    const std::optional<PadButton> lit = litButton();
    const std::optional<PadButton> mod = config_.modifier();

    const SDL_FColor edge = fcolor(150, 110, 40);
    const SDL_FColor body = fcolor(46, 42, 38);
    const SDL_FColor part = fcolor(78, 72, 64);
    const SDL_FColor partEdge = fcolor(120, 100, 70);
    const SDL_FColor well = fcolor(26, 24, 22);
    const SDL_FColor gold = fcolor(240, 180, 42);
    const SDL_FColor held = fcolor(120, 190, 255);
    const float line = std::max(1.0f, s * 0.8f);
    std::vector<SDL_FPoint> pts;

    const auto partColour = [&](PadButton b) {
        if (layer && mod && b == *mod) {
            return held;
        }
        return lit && *lit == b ? gold : part;
    };
    const auto spotBox = [&](const pad::PadSpot& sp, float grow, float radiusShare) {
        const SDL_FPoint c = at(sp.at.x, sp.at.y);
        const float rx = sp.rx * u + grow, ry = sp.ry * u + grow;
        pad::shape::roundBoxPoints(pts, c.x - rx, c.y - ry, 2 * rx, 2 * ry, std::min(rx, ry) * radiusShare);
    };
    const auto spot = [&](PadButton b) -> const pad::PadSpot* {
        for (const auto& sp : pad::padSpots()) {
            if (sp.button == b) {
                return &sp;
            }
        }
        return nullptr;
    };

    // The triggers and bumpers, behind the body's top edge.
    for (const PadButton b : {PadButton::LeftTrigger, PadButton::RightTrigger, PadButton::LeftShoulder, PadButton::RightShoulder}) {
        const pad::PadSpot* sp = spot(b);
        spotBox(*sp, 0, b == PadButton::LeftTrigger || b == PadButton::RightTrigger ? 0.8f : 1.0f);
        pad::shape::fill(r, pts, partColour(b));
        pad::shape::stroke(r, pts, line, partEdge);
    }
    // The body and grips: their edge (the shapes a little larger), then them.
    const pad::PadBody bd;
    for (const bool edgePass : {true, false}) {
        const float g = edgePass ? std::max(1.5f, s * 1.2f) : 0;
        const SDL_FColor c = edgePass ? edge : body;
        for (const auto* grip : {&bd.left, &bd.right}) {
            const SDL_FPoint gc = at(grip->at.x, grip->at.y);
            pad::shape::ellipsePoints(pts, gc.x, gc.y, grip->rx * u + g, grip->ry * u + g, grip->turn);
            pad::shape::fill(r, pts, c);
        }
        const SDL_FPoint tl = at(bd.x, bd.y);
        pad::shape::roundBoxPoints(pts, tl.x - g, tl.y - g, bd.w * u + 2 * g, bd.h * u + 2 * g, bd.radius * u + g);
        pad::shape::fill(r, pts, c);
    }
    // The sticks: a well, the top, a ring on it.
    for (const PadButton b : {PadButton::LeftStick, PadButton::RightStick}) {
        const pad::PadSpot* sp = spot(b);
        const SDL_FPoint c = at(sp->at.x, sp->at.y);
        pad::shape::circlePoints(pts, c.x, c.y, sp->rx * u * 1.3f);
        pad::shape::fill(r, pts, well);
        pad::shape::circlePoints(pts, c.x, c.y, sp->rx * u);
        pad::shape::fill(r, pts, partColour(b));
        pad::shape::stroke(r, pts, line, partEdge);
        pad::shape::circlePoints(pts, c.x, c.y, sp->rx * u * 0.62f);
        pad::shape::stroke(r, pts, line, fcolor(40, 36, 32));
    }
    // The D-pad: its cross (an edge round it), then an arm lit.
    {
        const SDL_FPoint c = at(0.37f, 0.44f);
        const float arm = 0.022f * u, reach = 0.098f * u;
        for (const bool edgePass : {true, false}) {
            const float g = edgePass ? line : 0;
            pad::shape::roundBoxPoints(pts, c.x - arm - g, c.y - reach - g, 2 * (arm + g), 2 * (reach + g), arm * 0.4f);
            pad::shape::fill(r, pts, edgePass ? partEdge : part);
            pad::shape::roundBoxPoints(pts, c.x - reach - g, c.y - arm - g, 2 * (reach + g), 2 * (arm + g), arm * 0.4f);
            pad::shape::fill(r, pts, edgePass ? partEdge : part);
        }
        for (const PadButton b : {PadButton::DpadUp, PadButton::DpadDown, PadButton::DpadLeft, PadButton::DpadRight}) {
            const SDL_FColor col = partColour(b);
            if (col.r != part.r || col.g != part.g) {
                spotBox(*spot(b), 0, 0.4f);
                pad::shape::fill(r, pts, col);
            }
        }
    }
    // The face buttons (their labels on them when the pad's known), View, Start and the middle one.
    for (const PadButton b : {PadButton::North, PadButton::South, PadButton::West, PadButton::East, PadButton::Guide}) {
        const pad::PadSpot* sp = spot(b);
        const SDL_FPoint c = at(sp->at.x, sp->at.y);
        const float rad = sp->rx * u;
        pad::shape::circlePoints(pts, c.x, c.y, rad);
        pad::shape::fill(r, pts, partColour(b));
        pad::shape::stroke(r, pts, line, partEdge);
        if (b == PadButton::Guide) {
            continue;
        }
        const pad::ButtonLabel l = label(b);
        if (l.glyph != pad::Glyph::Place) {
            pad::GlyphInk ink;
            const bool on = lit && *lit == b;
            ink.ink = on ? fcolor(30, 20, 6) : fcolor(230, 220, 200);
            ink.fill = on ? gold : part;
            pad::drawGlyph(r, font, l, c.x - rad, c.y, rad * 2, ink);
        }
    }
    for (const PadButton b : {PadButton::View, PadButton::Start}) {
        spotBox(*spot(b), 0, 1.0f);
        pad::shape::fill(r, pts, partColour(b));
        pad::shape::stroke(r, pts, line, partEdge);
    }

    // The callouts: a line to the button, its glyph and what it does on the layer shown.
    pad::GlyphInk ink;
    for (const pad::Callout& c : L.callouts) {
        const bool on = lit && *lit == c.button;
        const float lx = L.labelX(c);
        const float inner = c.left ? lx + L.labelW + 1 : lx - 1;
        const SDL_FColor lineColour = on ? gold : fcolor(150, 110, 40, 0.75f);
        pad::shape::line(r, out({inner, c.y}), out(c.anchor), on ? line * 1.5f : line, lineColour);
        const SDL_FPoint dot = out(c.anchor);
        pad::shape::circlePoints(pts, dot.x, dot.y, std::max(1.5f, s * 1.4f));
        pad::shape::fill(r, pts, lineColour);
        // What it does: the game's actions on it on that layer (the modifier says so).
        std::string what;
        if (mod && c.button == *mod) {
            what = pad::actionTitle(pad::padActionName(pad::PadAction::Modifier));
        } else {
            for (const auto& bnd : config_.bindings) {
                if (bnd.button == c.button && bnd.withModifier == layer && !pad::menuRow(bnd.action) &&
                    bnd.action != pad::padActionName(pad::PadAction::Modifier)) {
                    const std::string& t = pad::actionTitle(bnd.action);
                    if (what.find(t) == std::string::npos) {
                        what += (what.empty() ? "" : ", ") + t;
                    }
                }
            }
        }
        const pad::ButtonLabel l = label(c.button);
        const float gh = 12 * s;
        ink.ink = on ? fcolor(255, 232, 150) : gold;
        const float gw = pad::drawGlyph(r, font, l, o.x + lx * s, o.y + c.y * s, gh, ink) / s;
        const float tx = lx + gw + 3;
        const float tw = L.labelW - gw - 3;
        const std::string text = what.empty() ? "-" : what;
        const int lines = std::min<int>(2, font ? static_cast<int>(font->wrap(text, p.pixelSize(false), tw * s).size()) : 1);
        fittedText(p, text, tx, c.y - 5.0f * static_cast<float>(lines), tw, 2, what.empty() ? kDim : on ? kLight : kGold);
    }
    // The sticks' own ways, under the pad; the layer shown, over it.
    const float bottom = static_cast<float>(kPictureY + kPictureH);
    centredText(p, NOX_TR("controller.caption.left_stick", "Left stick: walk, and run when leant far"), kPictureX + kPictureW / 2.0f,
                bottom + 2, kDim);
    centredText(p, NOX_TR("controller.caption.right_stick", "Right stick: aim; a flick picks the next target"),
                kPictureX + kPictureW / 2.0f, bottom + 13, kDim);
    if (layer && mod) {
        centredText(p,
                    nox::l10n::format(NOX_TR("controller.caption.layer", "With {button} held"), {{"button", pad::labelWords(label(*mod))}}),
                    kPictureX + kPictureW / 2.0f, static_cast<float>(kPictureY) - 2, Color{120, 190, 255});
    }
}

void ControllerWindow::drawList(gui::Window& self, gui::Painter& p)
{
    SDL_Renderer* r = p.renderer();
    Font* font = p.font(false);
    const float s = p.scale();
    const SDL_FRect o = p.toOutput(0, 0, 0, 0);
    const SDL_Point at = self.screenPos();
    const int shown = self.rect.h / kLineStep;
    const auto pointed = pointedCell();
    const std::optional<PadButton> lit = litButton();
    const bool layer = layerShown();
    pad::GlyphInk ink;
    for (int i = self.scroll; i < static_cast<int>(lines_.size()) && i < self.scroll + shown; ++i) {
        const Line& line = lines_[static_cast<std::size_t>(i)];
        const int y = at.y + (i - self.scroll) * kLineStep;
        if (line.action.empty()) {
            // The menus' heading, with its cells' names.
            p.fill(at.x, y + kLineStep - 2, self.rect.w - 4, 1, Color{70, 52, 20});
            p.text(line.title, at.x + 4, y + 3, false, kGold);
            p.text(NOX_TR("controller.header.button", "Button"), at.x + kCell0 + 4, y + 3, false, kDim);
            p.text(NOX_TR("controller.header.second", "Or"), at.x + kCell1 + 4, y + 3, false, kDim);
            continue;
        }
        const bool here = pointed && pointed->first == i;
        bool match = false;
        for (int c = 0; c < 2 && lit; ++c) {
            const bool cellOnLayer = pad::menuRow(line.action) ? false : (c == 1) == layer;
            if (cellOnLayer && pad::cellButton(config_, line.action, c) == lit) {
                match = true;
            }
        }
        if (match) {
            p.fillAlpha(at.x, y, self.rect.w - 4, kLineStep - 1, {120, 88, 30, 120});
        } else if (here) {
            p.fillAlpha(at.x, y, self.rect.w - 4, kLineStep - 1, {60, 44, 18, 140});
        }
        fittedText(p, line.title, static_cast<float>(at.x + 4), static_cast<float>(y + 3), kCell0 - 8, 1, here ? kLight : kGold);
        for (int c = 0; c < 2; ++c) {
            if (!pad::cellUsable(line.action, c)) {
                continue;
            }
            const int cx = at.x + (c == 0 ? kCell0 : kCell1);
            const bool waiting = wait_.active() && captureLine_ == i && captureCell_ == c;
            if ((here && pointed->second == c) || waiting) {
                p.outline(cx, y, kCellW - 2, kLineStep - 1, waiting ? kLight : kGold);
            }
            if (const auto b = pad::cellButton(config_, line.action, c)) {
                ink.ink = here && pointed->second == c ? fcolor(255, 232, 150) : fcolor(240, 180, 42);
                pad::drawGlyph(r, font, label(*b), o.x + static_cast<float>(cx + 3) * s, o.y + (static_cast<float>(y) + 8) * s, 13 * s, ink);
            } else {
                p.text("-", cx + 6, y + 3, false, kDim);
            }
        }
    }
    // Where the list is scrolled to.
    const int count = static_cast<int>(lines_.size());
    if (count > shown) {
        const int trackH = self.rect.h;
        const int barH = std::max(12, trackH * shown / count);
        const int barY = at.y + (trackH - barH) * self.scroll / std::max(1, count - shown);
        p.fill(at.x + self.rect.w - 3, at.y, 2, trackH, Color{40, 30, 14});
        p.fill(at.x + self.rect.w - 3, barY, 2, barH, kBrown);
    }
}

void ControllerWindow::drawSettings(gui::Window& self, gui::Painter& p)
{
    const SDL_Point at = self.screenPos();
    const auto& s = config_.settings;
    char buf[32];
    const auto percent = [&](float f) {
        std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(f * 100)));
        return std::string(buf);
    };
    // (Each row's label over its control, top down: Use, the sliders, the other drop-downs.)
    const std::array<std::string, kRowCount> labels{
        NOX_TR("controller.setting.use", "Play With"),
        nox::l10n::format(NOX_TR("controller.setting.left_dead_zone", "Left Stick Dead Zone {value}"), {{"value", percent(s.deadZone)}}),
        nox::l10n::format(NOX_TR("controller.setting.right_dead_zone", "Right Stick Dead Zone {value}"), {{"value", percent(s.rightDeadZone)}}),
        nox::l10n::format(NOX_TR("controller.setting.run", "Run From a Lean of {value}"), {{"value", percent(s.runThreshold)}}),
        NOX_TR("controller.setting.aim_assist", "Aim Assist"),
        NOX_TR("controller.setting.lock", "Target Lock"),
        NOX_TR("controller.setting.prompts", "Button Names"),
    };
    static_assert(dropRow(kUseDrop) == 0 && sliderRow(0) == 1 && dropRow(0) == 1 + kSliderCount);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        fittedText(p, labels[i], static_cast<float>(at.x + 6), static_cast<float>(at.y + kRowY0 + kRowStep * static_cast<int>(i)),
                   kControlW, 1, kGold);
    }
}

void ControllerWindow::drawPopup(gui::Window& self, gui::Painter& p)
{
    const SDL_Point at = self.screenPos();
    p.fill(at.x, at.y, self.rect.w, self.rect.h, Color{0, 0, 0});
    const float cx = static_cast<float>(at.x) + self.rect.w / 2.0f;
    centredText(p, NOX_TR("controller.popup.press", "Press a button on the controller for"), cx, static_cast<float>(at.y + 8), kGold);
    std::string what;
    if (captureLine_ >= 0 && captureLine_ < static_cast<int>(lines_.size())) {
        const Line& l = lines_[static_cast<std::size_t>(captureLine_)];
        what = l.title;
        if (captureCell_ == 1 && !pad::menuRow(l.action)) {
            what = nox::l10n::format(NOX_TR("controller.popup.layer", "{action}, with {button} held"),
                                     {{"action", l.title}, {"button", pad::labelWords(label(config_.modifier().value_or(PadButton::LeftTrigger)))}});
        }
    }
    p.text(what, static_cast<int>(cx - textWidth(p, what) / 2), at.y + 26, true, kLight);
    // How to leave it: the Back button held (its glyph), or Escape.
    const std::uint32_t back = pad::backButtons(config_);
    std::optional<PadButton> b;
    for (std::size_t i = 0; i < pad::kPadButtonCount && !b; ++i) {
        if (back & (1u << i)) {
            b = static_cast<PadButton>(i);
        }
    }
    const std::string how = b ? nox::l10n::format(NOX_TR("controller.popup.cancel", "Hold {button} to leave it as it is, or press Escape."),
                                                  {{"button", pad::labelWords(label(*b))}})
                              : NOX_TR("controller.popup.cancel_key", "Escape leaves it as it is.");
    fittedText(p, how, static_cast<float>(at.x + 10), static_cast<float>(at.y + 56), self.rect.w - 20.0f, 2, kDim);
    const float share = wait_.cancelShare(nowMs_);
    p.fill(at.x + 20, at.y + self.rect.h - 18, self.rect.w - 40, 4, Color{40, 30, 14});
    if (share > 0) {
        p.fill(at.x + 20, at.y + self.rect.h - 18, static_cast<int>((self.rect.w - 40) * share), 4, kGold);
    }
}

} // namespace noxsdl
