// OptionsWindow (options_window.hpp): Options.wnd and its advanced panel, with noxsdl's own
// switches, mouse slider and drop-downs added to the panel, and the settings they change.
#include "options_window.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace noxsdl {

namespace {

namespace st = nox::gui::status;

// Options.wnd
constexpr std::int32_t kWindow65 = 311, kWindow75 = 312, kWindow85 = 313, kWindow100 = 314;
constexpr std::int32_t kRes640 = 321, kRes800 = 322, kRes1024 = 323, kRes1024Wide = 324;
constexpr std::int32_t kDepth8 = 331, kDepth16 = 332;
constexpr std::int32_t kInputConfig = 341;
constexpr std::int32_t kFxVolume = 351, kDialogVolume = 352, kMusicVolume = 353;
constexpr std::int32_t kFxOn = 361, kDialogOn = 362, kMusicOn = 363;
constexpr std::int32_t kClose = 371;
// AdVidOpt.wnd
constexpr std::int32_t kAdvancedClose = 2099;
constexpr std::int32_t kAdvancedTemplate = 2040; // Render GUI: the checkbox noxsdl's are copied from
// noxsdl's switches, added below them
constexpr std::int32_t kFrameRate = 2900, kBloom = 2901, kPerPixelLight = 2902, kSmoothEffects = 2903,
                       kHdImages = 2905, kMouseAccel = 2906, kKillFeed = 2907, kVsync = 2908, kExactMaths = 2909;
/// Those switches in the order they're stacked: their label (not a string-table id) and the
/// Extras field each turns on and off.
using Extras = OptionsWindow::Extras;
struct Switch {
    std::int32_t id;
    const char* label;
    bool Extras::*field;
};
constexpr std::array<Switch, 9> kSwitches{{
    {kFrameRate, "Frame Rate", &Extras::frameRate},
    {kBloom, "Bloom", &Extras::bloom},
    {kPerPixelLight, "Pixel Light", &Extras::perPixelLight},
    {kSmoothEffects, "Smooth FX", &Extras::smoothEffects},
    {kHdImages, "Use HD Images", &Extras::hdImages},
    {kKillFeed, "Kill Feed", &Extras::killFeed},
    {kMouseAccel, "Win Mouse Accel", &Extras::mouseAccel},
    {kVsync, "VSync", &Extras::vsync},
    {kExactMaths, "Exact Original Maths", &Extras::exactMaths},
}};
// Mouse speed: its label (with the speed) and slider (Extras::mouseSpeed, percent; with the 3D
// view on, Extras::mouseSpeed3D)
constexpr std::int32_t kMouseLabel = 2920, kMouseSlider = 2921;
// Smoothing: its label, the drop-down's button and its three choices (Extras::smoothing)
constexpr std::int32_t kSmoothLabel = 2910, kSmoothBox = 2911, kSmoothItem = 2912;
constexpr std::array<const char*, 4> kSmoothNames{"Off", "Smooth Movement", "Smooth Network", "Lock to 30 FPS"};
// Display: the same (Extras::display)
constexpr std::int32_t kDisplayLabel = 2930, kDisplayBox = 2931, kDisplayItem = 2932;
constexpr std::array<const char*, 3> kDisplayNames{"Windowed", "Fullscreen Windowed", "Fullscreen"};
// Weather Effects: the same (Extras::weather), on the More page
constexpr std::int32_t kWeatherLabel = 2950, kWeatherBox = 2951, kWeatherItem = 2952;
constexpr std::array<const char*, 3> kWeatherNames{"On", "Reduced", "Off"};
// The panel's tabs (noxsdl's), and the Server Settings' Game tab whose look they copy
// (servopts.wnd: a radio button drawn with UITabs3, lit UITabs1)
constexpr std::int32_t kTabGeneral = 2940, kTabMore = 2941;
constexpr std::int32_t kServOptsGameTab = 10160;
constexpr int kTabHeight = 20;
constexpr int kTabGroup = 0x7A;
constexpr int kPageTop = 3 + kTabHeight + 2; // the pages' first row, under the tabs

/// The page a row of the panel is on: the rarely changed switches on the second (More).
int pageOf(std::int32_t id)
{
    return id == 2012 || id == 2016 || id == kAdvancedTemplate || id == kFrameRate || id == kExactMaths || id == kWeatherLabel ||
                   id == kWeatherBox
               ? 1
               : 0;
}

struct Resolution {
    std::int32_t id;
    int width, height;
};
constexpr std::array<Resolution, 4> kResolutions{{
    {kRes640, 640, 480}, {kRes800, 800, 600}, {kRes1024, 1024, 768}, {kRes1024Wide, 1024, 576}}};
constexpr std::array<std::pair<std::int32_t, int>, 4> kWindowSizes{
    {{kWindow65, 65}, {kWindow75, 75}, {kWindow85, 85}, {kWindow100, 100}}};

/// AdVidOpt.wnd's checkboxes and the nox.cfg settings they show (client_advVideoOptsLoad), and whether noxsdl
/// acts on them; the others (the old renderer's) aren't shown. Front walls are always
/// translucent here (map_view.cpp).
struct Advanced {
    std::int32_t id;
    std::string_view setting; ///< empty: the game keeps it elsewhere (not in nox.cfg)
    bool acts;
};
constexpr std::array<Advanced, 14> kAdvanced{{
    {2010, "UnlockSurface", false},        // ClipWalls
    {2012, "SoftShadowEdge", true},        // titled Soft Shadow Edge (engine flag 0x400): the sight's soft edge
    {2014, "TranslucentConsole", false},
    {2015, "RenderGlow", true},            // the effects' glows
    {2016, "FadeObjects", true},           // what's seen that leaves sight fades out over a second
    {2017, "", false},                     // ShowTooltips
    {2020, "DrawFrontWalls", false},
    {2021, "TranslucentFrontWalls", false},
    {2022, "HighResFrontWalls", false},    // InterlacedFrontWalls
    {2031, "HighResFloors", false},        // InterlacedFloors
    {2032, "LockHighResFloors", false},
    {2033, "FlatShadedFloors", false},
    {2041, "RenderBubbles", true},         // chat bubbles
    {2040, "RenderGUI", true},             // the HUD
}};

// A config setting as a number (`fallback` when unset or empty).
int number(const std::optional<std::string>& s, int fallback)
{
    return s && !s->empty() ? std::atoi(s->c_str()) : fallback;
}

/// noxsdl's switches' tooltips: what each does, then its upside (+) and downside (-).
std::string_view extraTooltip(std::int32_t id)
{
    switch (id) {
    case kFrameRate:
        return "The frame rate and frame time in the top right\n(with Smooth Motion, its delay too).";
    case kBloom:
        return "A soft glow round bright effects and lights.\n+ Spells and fire look brighter.\n- A little more work for the graphics card.";
    case kPerPixelLight:
        return "Lights shade every pixel, smoothly.\n+ Smooth lighting with no blocky edges.\n- Off: the original's coarser light grid.";
    case kSmoothEffects:
        return "Effects drawn smooth at your screen's resolution,\nsmoke and poison as soft clouds, and Force Field as a\nglass barrier.\n+ Cleaner sparks, rays and clouds.\n- Off: the original's sprites.";
    case kSmoothLabel:
    case kSmoothBox:
        return "How movement between the server's 30 updates a second is drawn.\n"
               "Off: in the server's 30 steps a second, at your frame rate.\n"
               "Smooth Movement: each step spread over the frames between updates.\n"
               "Smooth Network: everything drawn between updates (adds latency).\n"
               "Lock to 30 FPS: as the original, a frame for each update.";
    case kSmoothItem:
        return "Off: everything is drawn where the server last said,\n"
               "in its 30 steps a second, with no added delay.\n"
               "- Above 30 FPS the steps can look uneven.";
    case kSmoothItem + 1:
        return "Smooth Movement: when the server moves something, it glides\n"
               "there over one update (~33 ms) instead of jumping.\n"
               "+ Smooth at any frame rate, never past where the server says.\n"
               "- About 17 ms behind on average (at most one update).";
    case kSmoothItem + 2:
        return "Smooth Network: everything you see is drawn smoothly\n"
               "between the server's updates.\n"
               "+ The smoothest picture: best for roleplay, video capture\n"
               "  and for-fun servers.\n"
               "- Everything is shown about 50 ms late (adapts to your\n"
               "  connection), on top of your ping: lead moving targets.";
    case kSmoothItem + 3:
        return "Lock to 30 FPS: a frame is drawn as each server update\n"
               "comes, as the original's 30 FPS loop showed them.\n"
               "+ Every step shown evenly once, no added delay.\n"
               "- 30 frames a second.";
    case kDisplayLabel:
    case kDisplayBox:
        return "How the game is shown: in a window, filling the screen\n"
               "as a borderless window, or fullscreen.";
    case kDisplayItem:
        return "Windowed: a window you can move and resize.";
    case kDisplayItem + 1:
        return "Fullscreen Windowed: a borderless window the size of the screen.\n"
               "+ Alt+Tab is instant.\n"
               "- The desktop's compositor stays on.";
    case kDisplayItem + 2:
        return "Fullscreen: the screen is the game's alone, at your\n"
               "desktop's resolution and refresh rate.\n"
               "+ The least latency.\n"
               "- Alt+Tab switches the screen's mode.";
    case kVsync:
        return "Frames wait for the screen's refresh.\n"
               "+ No tearing.\n"
               "- Up to a refresh's more latency.\n"
               "Off: as many frames as the computer can draw.";
    case kHdImages:
        return "Images from the HD image pack (video_hd.nxp) where it has them.\n- Greyed without a pack.";
    case kKillFeed:
        return "Who killed whom and with what, centred at the top of the\nscreen for a few seconds, as the original shows it.\nOff: in the console only.";
    case kMouseAccel:
        return "Windows' pointer speed and acceleration (Enhance Pointer\n"
               "Precision) on the mouse's raw movement.\n"
               "Off: the raw movement alone, one count a game pixel,\n"
               "as the original read it (at Mouse x1.00).";
    case kMouseLabel:
    case kMouseSlider:
        return "How fast the cursor moves: x0.25 to x3. At x1.00 one count\n"
               "of the mouse moves the cursor one pixel of the game's screen,\n"
               "as the original. The cursor stays in the window while the\n"
               "game has the focus, as the original's did.";
    case kExactMaths:
        return "Rounds the game's maths exactly as the original game did.\n"
               "Off, damage after armour, potion healing, carry capacity,\n"
               "experience and wear can differ by a point now and then, and\n"
               "movement, missile paths and sight edges very slightly.\n"
               "+ For campaign and map-script players who want the\n"
               "  original's exact behaviour.\n"
               "For games you host and solo or campaign play, not games you join.\n"
               "Takes effect from the next game.";
    case kWeatherLabel:
    case kWeatherBox:
        return "Rain, snow, storms and wind that a mod's game turns on,\n"
               "drawn over the world (they never change the game).\n"
               "- Greyed without the GPU renderer.";
    case kWeatherItem:
        return "On: the weather as the mod sets it, with lightning\n"
               "flashes in storms.";
    case kWeatherItem + 1:
        return "Reduced: fewer raindrops and flakes, and soft, gentle\n"
               "lightning with no flicker.\n"
               "+ Easier on slow graphics cards and on the eyes.";
    case kWeatherItem + 2:
        return "Off: no weather is drawn.";
    case kTabGeneral:
        return "The everyday settings.";
    case kTabMore:
        return "Settings seldom changed: the line of sight's soft edge,\n"
               "Fade Objects, the HUD, the frame-rate counter, the\n"
               "original's exact maths for games you host and the\n"
               "weather effects.";
    default:
        return {};
    }
}

} // namespace

std::string OptionsWindow::tooltip() const
{
    if (!open_) {
        return {};
    }
    const gui::Window* w = gui_.hovered();
    return w ? std::string(extraTooltip(w->id)) : std::string();
}
void OptionsWindow::setExtraEnabled(std::int32_t id, bool enabled)
{
    // The game's checkboxes only dim their light when off; noxsdl's own switches grey
    // their label too, so one that can't be used now shows it.
    if (gui::Window* w = advanced_ ? advanced_->find(id) : nullptr) {
        gui::setEnabled(w, enabled);
        w->textColor = enabled ? extraText_ : nox::gui::Color{110, 110, 110};
    }
}

// The mouse label: the speed as a multiple (the 3D mouse look's while the 3D view is on).
void OptionsWindow::setMouseText()
{
    if (gui::Window* w = advanced_ ? advanced_->find(kMouseLabel) : nullptr) {
        char text[32];
        std::snprintf(text, sizeof text, extras_.view3d ? "3D Mouse x%.2f" : "Mouse x%.2f",
                      extras_.sliderSpeed() / 100.0);
        w->text = text;
    }
}

// The drop-downs: their label, button and first choice's ids, their names, and the Extras field.
const std::array<OptionsWindow::DropDown, 3>& OptionsWindow::drops()
{
    static const std::array<DropDown, 3> kDrops{{
        {kSmoothLabel, kSmoothBox, kSmoothItem, kSmoothNames, &Extras::smoothing},
        {kDisplayLabel, kDisplayBox, kDisplayItem, kDisplayNames, &Extras::display},
        {kWeatherLabel, kWeatherBox, kWeatherItem, kWeatherNames, &Extras::weather},
    }};
    return kDrops;
}

void OptionsWindow::showDrop(const DropDown& d, bool open)
{
    for (int i = 0; i < static_cast<int>(d.names.size()); ++i) {
        if (gui::Window* w = advanced_ ? advanced_->find(d.item + i) : nullptr) {
            w->show(open);
            // the one in use lit as selected
            w->state = i == extras_.*d.value ? (w->state | gui::kPressed) : (w->state & ~gui::kPressed);
        }
    }
    // The choices are drawn over the rows below (the panel's first children) but a click finds
    // the last children first: a drop-down's button the list covers is off while it's open.
    const gui::Window* box = advanced_ ? advanced_->find(d.box) : nullptr;
    for (const DropDown& other : drops()) {
        gui::Window* below = advanced_ && other.box != d.box ? advanced_->find(other.box) : nullptr;
        if (box && below && below->rect.y > box->rect.y) {
            if (open) {
                below->status &= ~nox::gui::status::Enabled;
            } else {
                below->status |= nox::gui::status::Enabled;
            }
        }
    }
}

void OptionsWindow::setDropText(const DropDown& d)
{
    if (gui::Window* w = advanced_ ? advanced_->find(d.box) : nullptr) {
        w->text = d.names[static_cast<std::size_t>(std::clamp(extras_.*d.value, 0, static_cast<int>(d.names.size()) - 1))];
    }
}

OptionsWindow::OptionsWindow(gui::Resources res, std::filesystem::path windowDir, Callbacks callbacks)
    : gui_(std::move(res)), windowDir_(std::move(windowDir)), callbacks_(std::move(callbacks))
{
}

void OptionsWindow::open(const nox::config::Config& config, bool inGame, const Extras& extras, const SoundOn& sound)
{
    if (open_) {
        return;
    }
    config_ = config;
    extras_ = extras;
    sound_ = sound;
    inGame_ = inGame;
    open_ = true;
    const auto proc = [this](gui::Window&, int message, gui::Window& from) { return onMessage(message, from); };
    auto optionsDef = nox::gui::loadWindowFile(windowDir_ / "Options.wnd");
    // The widescreen 1024x576 button is only in some installs' Options.wnd (and its label only in
    // their string table): without it, a copy of the 1024x768 button a row below that one.
    bool addedWide = false;
    if (!optionsDef.find(kRes1024Wide)) {
        const auto& kids = optionsDef.children;
        const auto r1024 = std::ranges::find(kids, kRes1024, &nox::gui::WindowDef::id);
        const auto r800 = std::ranges::find(kids, kRes800, &nox::gui::WindowDef::id);
        if (r1024 != kids.end()) {
            nox::gui::WindowDef wide = *r1024;
            wide.id = kRes1024Wide;
            wide.y += r800 != kids.end() && r800->y < r1024->y ? r1024->y - r800->y : r1024->height;
            optionsDef.children.insert(r1024 + 1, std::move(wide));
            addedWide = true;
        }
    }
    window_ = gui_.load(optionsDef, proc);
    if (gui::Window* wide = window_->find(kRes1024Wide); wide && (addedWide || wide->text.empty())) {
        wide->text = "1024 X 576"; // (as the string table that has it writes it)
    }
    // The advanced options: the old renderer's switches for low-end hardware (surfaces, shading,
    // interlacing, front walls...) don't apply to noxsdl's and are left out; the ones it acts
    // on stay, with noxsdl's own switches after them, one a row 20 apart as the panel has them.
    auto advancedDef = nox::gui::loadWindowFile(windowDir_ / "advidopt.wnd");
    const auto templ = std::ranges::find(advancedDef.children, kAdvancedTemplate, &nox::gui::WindowDef::id);
    if (templ != advancedDef.children.end()) {
        const nox::gui::WindowDef base = *templ;
        // The checkboxes noxsdl doesn't act on dropped, the rest restacked a row each.
        std::erase_if(advancedDef.children, [](const nox::gui::WindowDef& d) {
            const auto a = std::ranges::find(kAdvanced, d.id, &Advanced::id);
            return a != kAdvanced.end() && !a->acts;
        });
        // Two pages under the tabs, each its rows 20 apart: the rarely changed switches on the
        // second (pageOf), the rest on the first.
        paged_.clear();
        std::array<int, 2> rows{};
        const auto place = [&](std::int32_t id, int page) {
            paged_.emplace_back(id, page);
            return kPageTop + 20 * rows[static_cast<std::size_t>(page)]++;
        };
        for (auto& d : advancedDef.children) {
            if (std::ranges::find(kAdvanced, d.id, &Advanced::id) != kAdvanced.end()) {
                d.y = place(d.id, pageOf(d.id));
            }
        }
        // noxsdl's switches: copies of the Render GUI checkbox, a row each below.
        for (const Switch& s : kSwitches) {
            nox::gui::WindowDef d = base;
            d.id = s.id;
            d.y = place(s.id, pageOf(s.id));
            d.text.clear();
            advancedDef.children.push_back(std::move(d));
        }
        // The tabs: copies of the Server Settings' Game tab (the game's tab art), overlapping as
        // its two do, along the panel's top.
        const auto servOpts = nox::gui::loadWindowFile(windowDir_ / "servopts.wnd");
        if (const nox::gui::WindowDef* tab = servOpts.find(kServOptsGameTab)) {
            for (const auto& [id, x] : {std::pair{kTabGeneral, 0}, std::pair{kTabMore, 85}}) {
                nox::gui::WindowDef t = *tab;
                t.id = id;
                t.x = x;
                t.y = 0;
                t.height = kTabHeight;
                t.status = st::Image | st::Enabled;
                t.group = kTabGroup; // (a group of their own: a click turns only the other off)
                t.font = base.font;  // (the panel's large font in the menus and in a game alike)
                t.text.clear();
                t.tooltip.clear();
                t.children.clear();
                advancedDef.children.push_back(std::move(t));
            }
        }
        // The template stripped of its pictures: the base of the label, slider and drop-down buttons below.
        nox::gui::WindowDef plain = base;
        plain.backgroundImage.clear();
        plain.enabledImage.clear();
        plain.disabledImage.clear();
        plain.hiliteImage.clear();
        plain.selectedImage.clear();
        plain.text.clear();
        plain.data = {};
        // A transparent static text, for the rows' labels.
        nox::gui::WindowDef label = plain;
        label.id = kSmoothLabel;
        label.type = nox::gui::WindowType::StaticText;
        label.typeName = "STATICTEXT";
        label.status = st::Enabled | st::NoFocus;
        label.style = nox::gui::style::StaticText;
        label.data = nox::gui::StaticTextData{};
        label.width = 64;
        const nox::gui::Color none{0, 0, 0, true};
        label.enabledColor = label.disabledColor = label.backgroundColor = label.hiliteColor = none;
        // The mouse's speed: a label with the speed, and a slider across (the game's are all
        // upright volume sliders, too tall for a row).
        const int mouseY = place(kMouseLabel, 0);
        paged_.emplace_back(kMouseSlider, 0);
        nox::gui::WindowDef mouseLabel = label;
        mouseLabel.id = kMouseLabel;
        mouseLabel.y = mouseY;
        mouseLabel.width = 90;
        advancedDef.children.push_back(mouseLabel);
        nox::gui::WindowDef slider = plain;
        slider.id = kMouseSlider;
        slider.type = nox::gui::WindowType::HorzSlider;
        slider.typeName = "HORZSLIDER";
        slider.status = st::Enabled | st::NoFocus;
        slider.style = nox::gui::style::HorzSlider | nox::gui::style::MouseTrack;
        slider.data = nox::gui::SliderData{kMouseSpeedMin, kMouseSpeedMax};
        slider.x = base.x + 92;
        slider.y = mouseY + 1;
        slider.width = 88;
        slider.height = 13;
        slider.enabledColor = {150, 110, 40};
        slider.hiliteColor = {240, 180, 42};
        slider.backgroundColor = {16, 12, 8};
        slider.selectedColor = {240, 180, 42};
        slider.disabledColor = {24, 24, 24};
        advancedDef.children.push_back(slider);
        // Smoothing and Display: the game has no combo box, so a label, a button showing the
        // choice and the choices as buttons under it while it's open, in the panel's colours.
        // (The choices go first in the panel: its first child is on top, drawn last, so an open
        // list covers the rows below it; the mouse finds the last first, so showDrop turns the
        // other drop-down's button off under it.)
        std::vector<nox::gui::WindowDef> lists;
        for (const DropDown& d : drops()) {
            const int y = place(d.label, pageOf(d.label));
            paged_.emplace_back(d.box, pageOf(d.box)); // (its choices: shown only while it's open, showDrop)
            // (Display's label is short and its choices long: its button starts further left;
            // Weather Effects' the other way round.)
            const int labelWidth = d.box == kDisplayBox ? 46 : d.box == kWeatherBox ? 108 : 64;
            nox::gui::WindowDef dropLabel = label;
            dropLabel.id = d.label;
            dropLabel.y = y;
            dropLabel.width = labelWidth;
            advancedDef.children.push_back(dropLabel);
            nox::gui::WindowDef box = plain;
            box.id = d.box;
            box.type = nox::gui::WindowType::PushButton;
            box.typeName = "PUSHBUTTON";
            box.status = st::Enabled | st::NoFocus;
            box.style = nox::gui::style::PushButton | nox::gui::style::MouseTrack;
            box.x = base.x + labelWidth + 2;
            box.y = y - 1;
            box.width = 180 - labelWidth - 2;
            box.height = 17;
            box.enabledColor = {150, 110, 40};
            box.hiliteColor = {240, 180, 42};
            box.backgroundColor = {16, 12, 8};
            box.selectedColor = {70, 52, 20};
            box.disabledColor = {24, 24, 24};
            advancedDef.children.push_back(box);
            for (int i = 0; i < static_cast<int>(d.names.size()); ++i) {
                nox::gui::WindowDef item = box;
                item.id = d.item + i;
                item.y = box.y + 17 * (i + 1);
                item.status |= st::Hidden;
                lists.push_back(item);
            }
        }
        advancedDef.children.insert(advancedDef.children.begin(), std::make_move_iterator(lists.begin()),
                                    std::make_move_iterator(lists.end()));
    }
    advanced_ = gui_.load(advancedDef, proc);
    for (const Switch& s : kSwitches) {
        if (gui::Window* w = advanced_->find(s.id)) {
            w->text = s.label; // (not string-table ids)
        }
    }
    for (const auto& [id, label] :
         {std::pair{kSmoothLabel, "Smoothing"}, std::pair{kDisplayLabel, "Display"}, std::pair{kWeatherLabel, "Weather Effects"}}) {
        if (gui::Window* w = advanced_->find(id)) {
            w->text = label;
        }
    }
    for (const DropDown& d : drops()) {
        if (gui::Window* box = advanced_->find(d.box)) {
            box->dropArrow = true; // (the game's list-box arrow on the button: it opens a list)
        }
        for (int i = 0; i < static_cast<int>(d.names.size()); ++i) {
            if (gui::Window* w = advanced_->find(d.item + i)) {
                w->text = d.names[static_cast<std::size_t>(i)];
            }
        }
    }
    for (const auto& [id, label] : {std::pair{kTabGeneral, "General"}, std::pair{kTabMore, "More"}}) {
        if (gui::Window* w = advanced_->find(id)) {
            w->text = label;
        }
    }
    advanced_->status |= nox::gui::status::Above; // (a child of the Options window in the game, over it)
    if (gui::Window* close = advanced_->find(kAdvancedClose)) {
        close->show(false); // (the Options window's own close button closes both)
    }

    // The resolution in use, and the game window's size (game_initOptionsInGame).
    int width = 1024, height = 576;
    // VideoMode reads "width height depth".
    if (const auto mode = config_.setting("VideoMode")) {
        char* end = nullptr;
        const long w = std::strtol(mode->c_str(), &end, 10);
        const long h = std::strtol(end, nullptr, 10);
        if (w > 0 && h > 0) {
            width = static_cast<int>(w);
            height = static_cast<int>(h);
        }
    }
    for (const auto& r : kResolutions) {
        gui::Window* w = window_->find(r.id);
        const bool current = r.width == width && r.height == height;
        gui::setChecked(w, current);
        // (game_initOptionsInGame leaves only the resolution in use on in a game; ours can be changed in a
        // game too, a deliberate deviation.)
        gui::setEnabled(w, true);
    }
    const int size = number(config_.setting("VideoSize"), 100);
    for (const auto& [id, percent] : kWindowSizes) {
        gui::setChecked(window_->find(id), percent == size);
    }
    gui::setChecked(window_->find(kDepth16), true);
    gui::setEnabled(window_->find(kDepth8), false); // (always 16-bit or better here)
    gui::setEnabled(window_->find(kDepth16), !inGame);

    // The volume sliders (0-16384, as game_initOptionsInGame sets them) with their lit thumbs.
    const std::array<std::pair<std::int32_t, std::string_view>, 3> sliders{
        {{kFxVolume, "FXVolume"}, {kDialogVolume, "DialogVolume"}, {kMusicVolume, "MusicVolume"}}};
    for (const auto& [id, name] : sliders) {
        if (gui::Window* s = window_->find(id)) {
            if (auto* data = std::get_if<nox::gui::SliderData>(&s->data)) {
                data->min = 0;
                data->max = 0x4000;
            }
            s->status |= st::Image;
            s->enabledImage = "OptionsVolumeSlider";
            s->hiliteImage = "OptionsVolumeSliderLit";
            s->sliderValue = std::clamp(number(config_.setting(name), 0x4000), 0, 0x4000);
        }
    }
    gui::setChecked(window_->find(kFxOn), sound_[0]);
    gui::setChecked(window_->find(kDialogOn), sound_[1]);
    gui::setChecked(window_->find(kMusicOn), sound_[2]);

    for (const auto& a : kAdvanced) {
        gui::Window* w = advanced_->find(a.id);
        if (!w) {
            continue;
        }
        gui::setChecked(w, !a.setting.empty() && number(config_.setting(a.setting), 1) != 0);
    }

    if (const gui::Window* w = advanced_->find(kFrameRate)) {
        extraText_ = w->textColor; // (the switches' label colour, as the template has it)
    }
    for (const Switch& s : kSwitches) {
        gui::setChecked(advanced_->find(s.id), extras_.*s.field);
    }
    for (const DropDown& d : drops()) {
        setDropText(d);
    }
    if (gui::Window* s = advanced_->find(kMouseSlider)) {
        s->sliderValue = std::clamp(extras_.sliderSpeed(), kMouseSpeedMin, kMouseSpeedMax);
    }
    setMouseText();
    setExtraEnabled(kHdImages, extras_.hdAvailable); // greyed without an image pack
    for (const auto id : {kBloom, kPerPixelLight, kSmoothEffects, kWeatherBox}) {
        if (!extras_.gpu) {
            setExtraEnabled(id, false); // they need the GPU renderer's shaders (off with --no-gpu)
        }
    }

    showPage(0); // (it opens on General)

    // Centred across, at the top (game_initOptionsInGame), the advanced panel in its place in it.
    window_->rect.x = 0;
    window_->rect.y = 0;
}

void OptionsWindow::showPage(int page)
{
    if (!advanced_) {
        return;
    }
    for (const DropDown& d : drops()) {
        showDrop(d, false);
    }
    for (const auto& [id, on] : paged_) {
        if (gui::Window* w = advanced_->find(id)) {
            w->show(on == page);
        }
    }
    gui::setChecked(advanced_->find(kTabGeneral), page == 0);
    gui::setChecked(advanced_->find(kTabMore), page == 1);
    // The lit tab over the other where they overlap: the earlier child is drawn last.
    auto& kids = advanced_->children;
    const auto at = [&](std::int32_t id) { return std::ranges::find_if(kids, [id](const auto& c) { return c->id == id; }); };
    const auto lit = at(page == 1 ? kTabMore : kTabGeneral);
    const auto other = at(page == 1 ? kTabGeneral : kTabMore);
    if (lit != kids.end() && other != kids.end() && other < lit) {
        std::iter_swap(lit, other);
    }
}

void OptionsWindow::close()
{
    if (!open_) {
        return;
    }
    open_ = false;
    gui_.destroyAll();
    window_ = nullptr;
    advanced_ = nullptr;
    // Leaving: the main menu's slides out; in a game the in-game menu comes back
    // with a click.
    gui_.resources().sound(inGame_ ? 0x399 : 0x39B);
    if (callbacks_.onClose) {
        callbacks_.onClose(config_);
    }
}

void OptionsWindow::setting(std::string_view name, std::string value)
{
    config_.set(name, std::move(value));
    if (callbacks_.onChange) {
        callbacks_.onChange(config_);
    }
}

bool OptionsWindow::onMessage(int message, gui::Window& from)
{
    // windowOptionsProc.
    if (message == gui::SliderChanged && from.id == kMouseSlider) {
        // In steps of 5% (x1 easy to find again), applied as it moves. With the 3D view on, its
        // mouse look's speed (kept apart from the cursor's).
        const int speed = std::clamp((from.sliderValue + 2) / 5 * 5, kMouseSpeedMin, kMouseSpeedMax);
        from.sliderValue = speed;
        if (speed != extras_.sliderSpeed()) {
            extras_.sliderSpeed() = speed;
            setMouseText();
            if (callbacks_.onExtras) {
                callbacks_.onExtras(extras_);
            }
        }
        return true;
    }
    // The three volume sliders, written to the config as they move.
    if (message == gui::SliderChanged) {
        const std::string value = std::to_string(from.sliderValue);
        int part = -1;
        if (from.id == kFxVolume) {
            setting("FXVolume", value);
            part = 0;
        } else if (from.id == kDialogVolume) {
            setting("DialogVolume", value);
            part = 1;
        } else if (from.id == kMusicVolume) {
            setting("MusicVolume", value);
            part = 2;
        }
        if (part >= 0) {
            // windowOptionsProc: down to nothing switches the part off, up from it switches it back on.
            const bool on = from.sliderValue != 0;
            if (on != sound_[static_cast<std::size_t>(part)]) {
                sound_[static_cast<std::size_t>(part)] = on;
                gui::setChecked(window_->find(kFxOn + part), on);
                if (callbacks_.onSound) {
                    callbacks_.onSound(sound_);
                }
            }
            if (part == 0 && on) {
                gui_.resources().sound(0x300); // LeverToggle: the new level
            }
        }
        return true;
    }
    // The advanced panel's procedure (client_advVideoOptsProc) points with ShellSelect and clicks with
    // ButtonPress; the main menu's Options point with ShellSelect too, the in-game
    // one (windowOptionsProc) doesn't; both click with ShellClick.
    bool inAdvanced = false;
    // Whether the control is in the advanced panel (it or any of its parents).
    for (const gui::Window* w = &from; w; w = w->parent) {
        inAdvanced = inAdvanced || w == advanced_;
    }
    if (message == gui::Hover) {
        if (inAdvanced || !inGame_) {
            gui_.resources().sound(0x398);
        }
        return false;
    }
    if (message != gui::Clicked) {
        return false;
    }
    gui_.resources().sound(inAdvanced ? 0x2FE : 0x399);
    if (from.id == kTabGeneral || from.id == kTabMore) {
        showPage(from.id == kTabMore ? 1 : 0);
        return true;
    }
    // The drop-downs: a button opens and shuts its list (shutting the other), a choice takes and
    // shuts it, a click anywhere else shuts them.
    for (const DropDown& d : drops()) {
        if (from.id == d.box) {
            const gui::Window* first = advanced_->find(d.item);
            const bool opening = first && (first->status & st::Hidden);
            for (const DropDown& other : drops()) {
                showDrop(other, false);
            }
            showDrop(d, opening);
            return true;
        }
        if (from.id >= d.item && from.id < d.item + static_cast<std::int32_t>(d.names.size())) {
            extras_.*d.value = from.id - d.item;
            showDrop(d, false);
            setDropText(d);
            if (callbacks_.onExtras) {
                callbacks_.onExtras(extras_);
            }
            return true;
        }
    }
    for (const DropDown& d : drops()) {
        showDrop(d, false);
    }
    // The sound switches (FX, dialog, music: ids in that order, like sound_).
    if (from.id == kFxOn || from.id == kDialogOn || from.id == kMusicOn) {
        sound_[static_cast<std::size_t>(from.id - kFxOn)] = (from.state & gui::kPressed) != 0;
        if (callbacks_.onSound) {
            callbacks_.onSound(sound_);
        }
        return true;
    }
    // A window size or a resolution: written to the config at once.
    for (const auto& [id, percent] : kWindowSizes) {
        if (from.id == id) {
            setting("VideoSize", std::to_string(percent));
            return true;
        }
    }
    for (const auto& r : kResolutions) {
        if (from.id == r.id) {
            setting("VideoMode", std::to_string(r.width) + " " + std::to_string(r.height) + " 16");
            return true;
        }
    }
    // The stock switches that are kept write their config setting as 1 or 0.
    for (const auto& a : kAdvanced) {
        if (from.id == a.id && !a.setting.empty()) {
            setting(a.setting, (from.state & gui::kPressed) ? "1" : "0");
            return true;
        }
    }
    if (const auto s = std::ranges::find(kSwitches, from.id, &Switch::id); s != kSwitches.end()) {
        extras_.*s->field = (from.state & gui::kPressed) != 0;
        if (callbacks_.onExtras) {
            callbacks_.onExtras(extras_);
        }
        return true;
    }
    if (from.id == kClose) {
        deferred_.push_back([this] { close(); });
        return true;
    }
    if (from.id == kInputConfig) {
        deferred_.push_back([this] {
            close();
            if (callbacks_.onInputConfig) {
                callbacks_.onInputConfig();
            }
        });
        return true;
    }
    return false;
}

void OptionsWindow::update(std::uint64_t nowMs)
{
    gui_.update(nowMs);
    gui::runQueued(deferred_);
}

void OptionsWindow::draw(SDL_Renderer* renderer, int width, int height)
{
    if (open_) {
        gui_.draw(renderer, width, height);
    }
}

bool OptionsWindow::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!open_) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat) {
        close();
        return true;
    }
    gui_.handleEvent(event, width, height);
    return true;
}

} // namespace noxsdl
