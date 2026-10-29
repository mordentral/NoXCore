// The game's window system, for its menus: windows loaded from the .wnd files (noxcore
// gui/window_file) and drawn and driven the way GAME.EXE's window manager does it, in its
// 640 x 480 layout scaled to fit the window.
//
//   tree       each window's children in file order; they draw newest first (walks
//              the child list from the last added), so a file's first child ends up on top; the
//              mouse searches the same list, so the LAST child gets it first, and
//              a click only ENABLED ones. Top-level windows draw BELOW ones first, then the
//              rest, then ABOVE ones. HIDDEN windows and their children don't draw
//              or take input.
//   drawing    per type, as the exe's draw procedures: plain windows, push buttons (wndButtonDrawNoImg
//              and its image form), radio buttons wndRadioButtonDraw, check boxes, static text
//              wndStaticDrawNoImage, list boxes wndListboxDrawWithImage / wndListboxDrawNoImage, sliders
//              wndScrollBoxDraw, entry fields wndEditDrawNoImage; BORDER windows get the framed border.
//   input      buttons take the press (message 5) and click on release while pressed (6),
//              telling their parent (0x4007); MOUSETRACK ones light up while the mouse is on
//              them (0x11 / 0x12, telling the parent 0x4005 / 0x4006). Radio buttons clear the
//              others of their group; check boxes toggle. A window file's own procedure is
//              given to every USER window it creates, so a screen sees its buttons.
//              A clicked entry field takes the focus and gets the typing (text input events,
//              Backspace), up to its maximum length.
//   animation  the menus slide in and out (gui_makeAnimation).
#pragma once

#include "sprite_cache.hpp"
#include "text.hpp"

#include "noxcore/gui/window_file.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl::gui {

/// Window messages (the exe's numbers).
enum Message : int {
    MouseDown = 5,
    MouseUp = 6,
    MouseUpOutside = 7,
    MouseEnter = 0x11,
    MouseLeave = 0x12,
    // To a window's parent:
    Pressed = 0x4000,
    Hover = 0x4005,
    Unhover = 0x4006,
    Clicked = 0x4007,
    SliderChanged = 0x4009, ///< a slider's value moved (Window::sliderValue)
    EntryEntered = 0x401F,  ///< Enter in an entry field (wndEditProc)
};

/// State bits (the window's draw data word 0).
inline constexpr std::uint32_t kLit = 2;      ///< the mouse is on it
inline constexpr std::uint32_t kPressed = 4;  ///< held down, or checked

struct Resources {
    const nox::things::ThingDb& things;
    SpriteCache& sprites;
    Font* font = nullptr;
    const nox::strings::StringTable* strings = nullptr;
    /// Snd_PlaySound for the screens' own sounds (clicks, slides); may be empty.
    std::function<void(std::uint32_t soundId)> playSound;

    void sound(std::uint32_t id) const
    {
        if (playSound) {
            playSound(id);
        }
    }
};

class Window;
class Painter;

/// A list's first line for its vertical slider's `value` (the slider's maximum at the top, the
/// list's start), 0 to `most`, the last first line there can be.
inline int scrollFromSlider(const nox::gui::SliderData& sl, int value, int most)
{
    return std::clamp((sl.max - value) * most / std::max(1, sl.max - sl.min), 0, most);
}

/// The other way: the vertical slider's value for the list's first line `scroll` of `most`
/// (its maximum when there's nothing to scroll).
inline int sliderFromScroll(const nox::gui::SliderData& sl, int scroll, int most)
{
    return most > 0 ? sl.max - (sl.max - sl.min) * scroll / most : sl.max;
}

/// Runs the work a screen's button handlers queued, once no event is being handled. `queue` is
/// emptied first, so what runs may queue more (for the next time).
inline void runQueued(std::vector<std::function<void()>>& queue)
{
    auto work = std::move(queue);
    queue.clear();
    for (auto& f : work) {
        f();
    }
}

/// Whether thing.bin has an image by this name (gLoadImg finding it).
bool imageExists(const nox::things::ThingDb& things, const std::string& name);

/// client_gui_flag: on while the shell is up (MainBG loaded, wndLoadMainBG; back from a
/// game, cliWaitForJoinData), off in a game (client_xxx_switchChatMap). A window file's window with no FONT, there or in
/// its header, gets the "large" font while it's on, else "default", so the menus'
/// screens draw their text large and the game's windows small.
void setShellUp(bool up);
bool shellUp();

/// A dlg.wnd dialog's title (4005) and text (4004) in the "large" font for 1, in
/// "default" for 0. They have no FONT of their own, so without it they go by the shell (above).
void setDialogFonts(Window& dialog, bool large);

/// A window's procedure: gets the messages its children send it (Clicked, Hover...), with
/// the child. Returns whether it handled it.
using Proc = std::function<bool(Window& self, int message, Window& from)>;
/// Draws a window instead of its type's drawing (the exe's per-window draw functions).
using DrawFn = std::function<void(Window& self, Painter& painter)>;

class Window {
public:
    Window(const nox::gui::WindowDef& def, Window* parent, const Resources& res);

    std::int32_t id = 0;
    SDL_Rect rect{};         ///< relative to the parent
    nox::gui::WindowType type = nox::gui::WindowType::Unknown;
    std::uint32_t status = 0; ///< nox::gui::status
    std::uint32_t style = 0;  ///< nox::gui::style
    std::uint32_t state = 0;  ///< kLit, kPressed
    std::int32_t group = 0;
    nox::gui::Color enabledColor, disabledColor, hiliteColor, selectedColor, textColor, backgroundColor;
    std::string backgroundImage, enabledImage, disabledImage, hiliteImage, selectedImage;
    SDL_Point imageOffset{};
    bool largeFont = false;
    std::string text;    ///< shown (resolved from the string table)
    std::string tooltip;
    nox::gui::WindowData data;
    std::int32_t radioValue = 0; ///< RADIOBUTTON DATA: 0 text beside the button, else centred

    // List boxes.
    struct Line {
        std::string text;
        nox::gui::Color color;
        bool marked = false; ///< chosen in a list that takes several (drawn as the selected one)
        int height = 0;      ///< as last drawn: wrapped lines take more (wndListBoxAddLine)
    };
    std::vector<Line> lines;
    int selectedLine = -1;
    int scroll = 0; ///< the first line shown
    /// Where a tab in a line puts the rest (draw_setTabWidth's width).
    int tabWidth = 100;
    // Sliders.
    int sliderValue = 0;
    /// How long the thumb is along the track (the slider's thumb size; 10 unless set).
    int thumbLength = 10;
    // Progress bars: 0-100 (message 0x4020).
    int progress = 0;
    // Entry fields: the typed text, at most `entryMax` characters (the DATA's maxLength).
    std::u16string entry;
    std::size_t entryMax = 256;
    /// What the exe keeps in a widget's data word for windows it draws itself (SelColor's
    /// swatches: palette table in the low half, index in the high half).
    std::uint32_t userData = 0;
    /// noxsdl's drop-down buttons (the game has no combo box): a plain push button that also
    /// draws the game's list-box down arrow (UIScrollDn, lit while the mouse is on it) at its
    /// right end, its text centred in the rest.
    bool dropArrow = false;
    /// (noxcpp) Drawn visibly greyed while disabled: its texts, list lines and box outlines in a dim
    /// grey, its images darkened. Off (the stock look, every window the files
    /// make): a disabled window draws as the exe's does, often no different from an enabled one.
    bool dimDisabled = false;

    Window* parent = nullptr;
    std::vector<std::unique_ptr<Window>> children; ///< file order
    Proc proc;
    DrawFn draw;

    bool enabled() const { return status & nox::gui::status::Enabled; }
    bool hidden() const { return status & nox::gui::status::Hidden; }
    void show(bool visible);
    /// Position on the 640 x 480 screen.
    SDL_Point screenPos() const;
    /// This window or the first one with that id below it (depth first), or nullptr.
    Window* find(std::int32_t id);
    /// Tells the parent, as the exe's widgets do (the draw data's owner window).
    void notify(int message);
};

/// A check box or button (if there is one) checked (pressed: kPressed) or not.
inline void setChecked(Window* w, bool on)
{
    if (w) {
        w->state = on ? (w->state | kPressed) : (w->state & ~kPressed);
    }
}

/// Whether a check box or button is checked (pressed).
inline bool isChecked(const Window& w)
{
    return (w.state & kPressed) != 0;
}

/// A window (if there is one) enabled, taking clicks, or not.
inline void setEnabled(Window* w, bool on)
{
    if (w) {
        w->status = on ? (w->status | nox::gui::status::Enabled) : (w->status & ~nox::gui::status::Enabled);
    }
}

/// (noxcpp) A window (if there is one) and everything in it greyed out: disabled and drawn dimmed
/// (Window::dimDisabled), or back to usable and the stock look.
inline void setGreyed(Window* w, bool greyed)
{
    if (!w) {
        return;
    }
    setEnabled(w, !greyed);
    w->dimDisabled = greyed;
    for (auto& c : w->children) {
        setGreyed(c.get(), greyed);
    }
}

/// A window centred in the game's 640 x 480 layout.
inline void centreOnScreen(Window& w)
{
    w.rect.x = (640 - w.rect.w) / 2;
    w.rect.y = (480 - w.rect.h) / 2;
}

/// A slider (if there is one) given the thumb image UISlider, UISliderLit while lit.
inline void useSliderThumb(Window* slider)
{
    if (slider) {
        slider->enabledImage = "UISlider";
        slider->hiliteImage = "UISliderLit";
        slider->status |= nox::gui::status::Image;
    }
}

/// Draws in the game's 640 x 480 coordinates, scaled to the output.
class Painter {
public:
    Painter(SDL_Renderer* renderer, const Resources& res, int outputWidth, int outputHeight);
    /// With the layout at `scale` output pixels per unit and its corner at (offsetX, offsetY).
    Painter(SDL_Renderer* renderer, const Resources& res, float scale, float offsetX, float offsetY)
        : renderer_(renderer), res_(res), scale_(scale), offsetX_(offsetX), offsetY_(offsetY)
    {
    }

    void fill(int x, int y, int w, int h, const nox::gui::Color& c);
    /// A blended fill (the exe's alpha rectangles).
    void fillAlpha(int x, int y, int w, int h, SDL_Color c);
    void outline(int x, int y, int w, int h, const nox::gui::Color& c); ///< one-pixel border lines
    /// An image by name (thing.bin's named images; animations play), at its own offset.
    void image(const std::string& name, int x, int y);
    /// (noxcpp) The same darkened, `dim` set (a greyed control's: Window::dimDisabled); else as image.
    void image(const std::string& name, int x, int y, bool dim);
    /// The same with its material pixels shaded (draw_setMaterial's two forms).
    void image(const std::string& name, int x, int y, const SpriteCache::Materials& materials);
    int fontHeight(bool large) const { return large ? 16 : 10; }
    int textWidth(const std::string& utf8, bool large);
    /// The height of the text wrapped at `wrapWidth` (a font height a line).
    int textHeight(const std::string& utf8, bool large, int wrapWidth);
    /// Text with its top-left at (x, y), wrapped at `wrapWidth` (0: no wrapping).
    void text(const std::string& utf8, int x, int y, bool large, const nox::gui::Color& c, int wrapWidth = 0);
    /// The first line of the text wrapped at `wrapWidth` (drawStringWrap given one
    /// line's height).
    std::string firstLine(const std::string& utf8, bool large, int wrapWidth);
    /// A thing's image with its top-left at (x, y), at its own offset.
    void sprite(const nox::things::ImageRef& ref, int x, int y);
    /// A rectangle in output pixels, for things drawn in a window's place (our browser).
    SDL_FRect toOutput(int x, int y, int w, int h) const;
    float scale() const { return scale_; }
    /// The pixel size to draw text at.
    int pixelSize(bool large) const;
    SDL_Renderer* renderer() const { return renderer_; }
    /// Output pixels to the 640 x 480 layout.
    SDL_FPoint toLayout(float x, float y) const;

private:
    void drawSprite(const Sprite* s, int x, int y);

    SDL_Renderer* renderer_;
    const Resources& res_;
    float scale_ = 1;
    float offsetX_ = 0, offsetY_ = 0;
};

/// A window sliding in from `from` to its place, and back out when asked (gui_makeAnimation).
struct Slide {
    Window* window = nullptr;
    SDL_Point from{}, to{};
    int speedIn = 0, speedOut = 0; ///< game units a frame, along y (and x)
    SDL_Point speedInXY{}, speedOutXY{};
    bool out = false;
    bool done = false;
    std::function<void()> onOut; ///< once back at `from`
};

class Gui {
public:
    explicit Gui(Resources res) : res_(std::move(res)) {}

    /// Loads a window file as a new top-level window; every USER window in it gets `proc`.
    Window* load(const std::filesystem::path& file, Proc proc);
    /// The same from a definition already read (and perhaps added to).
    Window* load(const nox::gui::WindowDef& def, Proc proc);
    /// Destroys a top-level window and everything in it.
    void destroy(Window* top);
    void destroyAll();

    /// Only `w` (and its children) take input until it's destroyed or cleared with nullptr
    /// (wndShowModalMB).
    void setModal(Window* w) { modal_ = w; }
    Window* modal() const { return modal_; }
    /// Makes a child window a top-level one (with no parent); it stays where it is on
    /// screen. Returns it.
    Window* detach(Window* w);
    /// The window that gets typing (an entry field), or nullptr (wndGetFocus).
    Window* focus() const { return focus_; }
    /// The window under the mouse (as of the last mouse move), or nullptr.
    Window* hovered() const { return hover_; }
    /// The window the left button's press went to (the enabled one under it), till its release.
    Window* pressed() const { return pressed_; }
    void setFocus(Window* w) { focus_ = w; }

    /// Starts a slide (the window is moved to `from` now); returns it for slideOut.
    Slide* slideIn(Window* window, SDL_Point from, SDL_Point speedIn, SDL_Point speedOut);
    void slideOut(Slide* slide, std::function<void()> onOut);
    bool sliding() const;

    /// Where the 640 x 480 layout goes in a width x height output: {scale, x, y}. Unset, it's
    /// scaled to fit and centred (the menus); the in-game windows put it at the screen's bottom
    /// right at the game's scale, as the exe does at a bigger resolution.
    using Placement = std::function<std::array<float, 3>(int width, int height)>;
    void setPlacement(Placement p) { placement_ = std::move(p); }
    /// An output pixel in the layout's units.
    SDL_FPoint layoutPoint(float x, float y, int width, int height) const
    {
        return painter(nullptr, width, height).toLayout(x, y);
    }

    /// Steps the slides (30 a second).
    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// Mouse input; returns whether a window took it.
    bool handleEvent(const SDL_Event& event, int width, int height);

    const Resources& resources() const { return res_; }
    /// The string table text for an id ("MainMenu.wnd:Solo"), or the id. A bare label is also
    /// looked up as "<source>:<label>", as Str_GetString qualifies it with the calling source
    /// file ("LoadCharLabel" from selchar.c is "SelChar.c:LoadCharLabel").
    std::string text(const std::string& id, const std::string& source = {}) const;

private:
    void drawWindow(Window& w, Painter& p);
    void drawByType(Window& w, Painter& p);
    void drawBorder(Window& w, Painter& p);
    /// The window at (x, y); `press`: only ENABLED ones (a click's), else any (the hover's).
    Window* hit(Window& w, int x, int y, bool press);
    void dragSlider(Window& w, SDL_Point at);
    Window* hitTop(int x, int y, bool press);
    std::unique_ptr<Window> build(const nox::gui::WindowDef& def, Window* parent, const Proc& proc);

    Resources res_;
    std::vector<std::unique_ptr<Window>> tops_;
    std::vector<std::unique_ptr<Slide>> slides_;
    Window* hover_ = nullptr;
    Window* pressed_ = nullptr;
    Window* modal_ = nullptr;
    Window* focus_ = nullptr;
    std::uint64_t lastStepMs_ = 0;
    Placement placement_;
    Painter painter(SDL_Renderer* renderer, int width, int height) const;
};

} // namespace noxsdl::gui
