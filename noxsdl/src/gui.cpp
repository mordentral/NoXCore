// The window system's windows, painter, drawing and input (gui.hpp describes the design).
#include "gui.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::gui {

namespace st = nox::gui::status;
namespace sy = nox::gui::style;
using nox::gui::Color;
using nox::gui::WindowType;

namespace {

bool visible(const Color& c)
{
    return !c.transparent;
}

SDL_Color sdl(const Color& c)
{
    return {c.r, c.g, c.b, 255};
}

/// A list box's line height: its DATA's, raised to the font's (gui_newScrollListBox).
int lineHeight(const Window& w)
{
    const int fh = w.largeFont ? 16 : 10;
    const auto* d = std::get_if<nox::gui::ListBoxData>(&w.data);
    return std::max(fh, d ? static_cast<int>(d->lineHeight) : 0);
}

/// thing.bin's named images, by name.
const std::map<std::string, const nox::things::ImageDef*>& imageIndex(const nox::things::ThingDb& things)
{
    static const nox::things::ThingDb* built = nullptr;
    static std::map<std::string, const nox::things::ImageDef*> index;
    if (built != &things) {
        index.clear();
        for (const auto& i : things.images) {
            index.emplace(i.name, &i);
        }
        built = &things;
    }
    return index;
}

// client_gui_flag (setShellUp / shellUp).
bool shellFlag = false;

} // namespace

void setShellUp(bool up)
{
    shellFlag = up;
}

bool shellUp()
{
    return shellFlag;
}

void setDialogFonts(Window& dialog, bool large)
{
    for (const std::int32_t id : {4005, 4004}) {
        if (Window* w = dialog.find(id)) {
            w->largeFont = large;
        }
    }
}

// ------------------------------------------------------------------------------------ Window

// Copies a .wnd definition, resolving its text and tooltip ids through the string table.
Window::Window(const nox::gui::WindowDef& def, Window* parentWindow, const Resources& res)
    : id(def.id), rect{def.x, def.y, def.width, def.height}, type(def.type), status(def.status), style(def.style),
      group(def.group), enabledColor(def.enabledColor), disabledColor(def.disabledColor),
      hiliteColor(def.hiliteColor), selectedColor(def.selectedColor), textColor(def.textColor),
      backgroundColor(def.backgroundColor), backgroundImage(def.backgroundImage), enabledImage(def.enabledImage),
      disabledImage(def.disabledImage), hiliteImage(def.hiliteImage), selectedImage(def.selectedImage),
      imageOffset{def.imageOffsetX, def.imageOffsetY}, data(def.data), parent(parentWindow)
{
    // The font: "large", else the default ("default", "small"); none at all is the large one
    // while the shell is up.
    largeFont = nox::gui::usesLargeFont(def.font, shellUp());
    const auto resolve = [&](const std::string& textId) -> std::string {
        if (textId.empty() || !res.strings) {
            return textId;
        }
        const auto t = res.strings->text(textId);
        return t ? nox::strings::toUtf8(*t) : std::string{};
    };
    text = resolve(def.text);
    tooltip = resolve(def.tooltip);
    if (auto* st = std::get_if<nox::gui::StaticTextData>(&data)) {
        text = resolve(st->text);
    }
    if (const auto* rb = std::get_if<nox::gui::RadioButtonData>(&data)) {
        radioValue = rb->value;
    }
    if (const auto* sl = std::get_if<nox::gui::SliderData>(&data)) {
        sliderValue = sl->min;
    }
    if (const auto* ef = std::get_if<nox::gui::EntryFieldData>(&data)) {
        entryMax = ef->maxLength == 0 ? 256 : std::min<std::size_t>(ef->maxLength, 256);  // 0 in the file: the 256 limit
    }
}

void Window::show(bool visibleNow)
{
    if (visibleNow) {
        status &= ~st::Hidden;
    } else {
        status |= st::Hidden;
    }
}

SDL_Point Window::screenPos() const
{
    SDL_Point p{rect.x, rect.y};
    for (const Window* w = parent; w; w = w->parent) {
        p.x += w->rect.x;
        p.y += w->rect.y;
    }
    return p;
}

Window* Window::find(std::int32_t wanted)
{
    if (id == wanted) {
        return this;
    }
    for (auto& c : children) {
        if (Window* hit = c->find(wanted)) {
            return hit;
        }
    }
    return nullptr;
}

void Window::notify(int message)
{
    if (parent && parent->proc) {
        parent->proc(*parent, message, *this);
    }
}

// ----------------------------------------------------------------------------------- Painter

Painter::Painter(SDL_Renderer* renderer, const Resources& res, int outputWidth, int outputHeight)
    : renderer_(renderer), res_(res)
{
    // The 640 x 480 layout fitted inside the output keeping its shape, centred on whole pixels.
    scale_ = std::min(static_cast<float>(outputWidth) / 640.0f, static_cast<float>(outputHeight) / 480.0f);
    offsetX_ = std::floor((static_cast<float>(outputWidth) - 640 * scale_) / 2);
    offsetY_ = std::floor((static_cast<float>(outputHeight) - 480 * scale_) / 2);
}

SDL_FRect Painter::toOutput(int x, int y, int w, int h) const
{
    return {offsetX_ + static_cast<float>(x) * scale_, offsetY_ + static_cast<float>(y) * scale_,
            static_cast<float>(w) * scale_, static_cast<float>(h) * scale_};
}

Painter Gui::painter(SDL_Renderer* renderer, int width, int height) const
{
    if (placement_) {
        const auto [scale, x, y] = placement_(width, height);
        return Painter(renderer, res_, scale, x, y);
    }
    return Painter(renderer, res_, width, height);
}

SDL_FPoint Painter::toLayout(float x, float y) const
{
    return {(x - offsetX_) / scale_, (y - offsetY_) / scale_};
}

void Painter::fill(int x, int y, int w, int h, const Color& c)
{
    if (!visible(c) || w <= 0 || h <= 0) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 255);
    const SDL_FRect r = toOutput(x, y, w, h);
    SDL_RenderFillRect(renderer_, &r);
}

void Painter::outline(int x, int y, int w, int h, const Color& c)
{
    if (!visible(c) || w <= 0 || h <= 0) {
        return;
    }
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

void Painter::fillAlpha(int x, int y, int w, int h, SDL_Color c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    const SDL_FRect r = toOutput(x, y, w, h);
    SDL_RenderFillRect(renderer_, &r);
}

namespace {

/// A named image's picture now (its current frame if animated).
std::optional<nox::things::ImageRef> namedImage(const nox::things::ThingDb& things, const std::string& name)
{
    if (name.empty()) {
        return std::nullopt;
    }
    const auto& index = imageIndex(things);
    const auto it = index.find(name);
    if (it == index.end()) {
        return std::nullopt;
    }
    const nox::things::ImageDef& def = *it->second;
    std::optional<nox::things::ImageRef> ref = def.image;
    // Animated named images run on the wall clock at the game's 30 frames a second.
    if (def.animation && !def.animation->frames.empty()) {
        const auto& a = *def.animation;
        const auto frame = SDL_GetTicks() * 30 / 1000 / (a.frameDelay + 1u);
        ref = a.frames[frame % a.frames.size()];
    }
    return ref;
}

} // namespace

bool imageExists(const nox::things::ThingDb& things, const std::string& name)
{
    return namedImage(things, name).has_value();
}

void Painter::sprite(const nox::things::ImageRef& ref, int x, int y)
{
    drawSprite(res_.sprites.get(ref), x, y);
}

std::string Painter::firstLine(const std::string& utf8, bool large, int wrapWidth)
{
    if (!res_.font || utf8.empty() || wrapWidth <= 0) {
        return utf8;
    }
    const auto lines = res_.font->wrap(utf8, pixelSize(large), static_cast<float>(wrapWidth) * scale_);
    return lines.empty() ? std::string() : lines.front();
}

void Painter::image(const std::string& name, int x, int y)
{
    const auto ref = namedImage(res_.things, name);
    drawSprite(ref ? res_.sprites.get(*ref) : nullptr, x, y);
}

void Painter::image(const std::string& name, int x, int y, const SpriteCache::Materials& materials)
{
    const auto ref = namedImage(res_.things, name);
    drawSprite(ref ? res_.sprites.tinted(*ref, materials) : nullptr, x, y);
}

void Painter::drawSprite(const Sprite* s, int x, int y)
{
    if (!s) {
        return;
    }
    SDL_FRect dst{offsetX_ + (static_cast<float>(x) + s->offsetX) * scale_,
                  offsetY_ + (static_cast<float>(y) + s->offsetY) * scale_, s->width * scale_, s->height * scale_};
    s->scale = SDL_SCALEMODE_LINEAR;
    res_.sprites.render(*s, nullptr, dst);
}

int Painter::pixelSize(bool large) const
{
    // The TTF drawn so its line is as tall as the game's font at this scale.
    if (!res_.font) {
        return 10;
    }
    const float target = static_cast<float>(fontHeight(large)) * scale_;
    const float perPixel = res_.font->lineHeight(100) / 100.0f;
    return std::max(6, static_cast<int>(std::lround(target / perPixel)));
}

int Painter::textWidth(const std::string& utf8, bool large)
{
    if (!res_.font || utf8.empty()) {
        return 0;
    }
    return static_cast<int>(std::ceil(res_.font->measure(utf8, pixelSize(large)) / scale_));
}

int Painter::textHeight(const std::string& utf8, bool large, int wrapWidth)
{
    if (!res_.font || utf8.empty()) {
        return fontHeight(large);
    }
    const auto lines = wrapWidth > 0 ? res_.font->wrap(utf8, pixelSize(large), static_cast<float>(wrapWidth) * scale_).size()
                                     : std::size_t{1};
    return static_cast<int>(std::max<std::size_t>(lines, 1)) * fontHeight(large);
}

void Painter::text(const std::string& utf8, int x, int y, bool large, const Color& c, int wrapWidth)
{
    if (!res_.font || utf8.empty() || !visible(c)) {
        return;
    }
    const int size = pixelSize(large);
    std::vector<std::string> lines;
    if (wrapWidth > 0) {
        lines = res_.font->wrap(utf8, size, static_cast<float>(wrapWidth) * scale_);
    } else {
        lines.push_back(utf8);
    }
    float oy = offsetY_ + static_cast<float>(y) * scale_;
    for (const auto& line : lines) {
        res_.font->draw(line, offsetX_ + static_cast<float>(x) * scale_, oy, size, sdl(c));
        oy += static_cast<float>(fontHeight(large)) * scale_;
    }
}

// --------------------------------------------------------------------------------------- Gui

std::string Gui::text(const std::string& id, const std::string& source) const
{
    if (!res_.strings) {
        return id;
    }
    if (const auto t = res_.strings->text(id)) {
        return nox::strings::toUtf8(*t);
    }
    if (!source.empty() && id.find(':') == std::string::npos) {
        if (const auto t = res_.strings->text(source + ":" + id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return id;
}

// Builds a window and its children from a definition, depth first.
std::unique_ptr<Window> Gui::build(const nox::gui::WindowDef& def, Window* parent, const Proc& proc)
{
    auto w = std::make_unique<Window>(def, parent, res_);
    if (def.type == WindowType::User) {
        w->proc = proc; // the file's procedure for its USER windows
    }
    for (const auto& c : def.children) {
        w->children.push_back(build(c, w.get(), proc));
    }
    return w;
}

Window* Gui::load(const std::filesystem::path& file, Proc proc)
{
    return load(nox::gui::loadWindowFile(file), std::move(proc));
}

Window* Gui::load(const nox::gui::WindowDef& def, Proc proc)
{
    tops_.push_back(build(def, nullptr, proc));
    return tops_.back().get();
}

// Destroys a top-level window: its slides go, hover and press are cleared, and so are the
// modal and the focus if they were in it.
void Gui::destroy(Window* top)
{
    std::erase_if(slides_, [&](const auto& s) {
        for (const Window* w = s->window; w; w = w->parent) {
            if (w == top) {
                return true;
            }
        }
        return false;
    });
    hover_ = nullptr;
    pressed_ = nullptr;
    if (modal_ == top) {
        modal_ = nullptr;
    }
    for (const Window* w = focus_; w; w = w->parent) {
        if (w == top) {
            focus_ = nullptr;
            break;
        }
    }
    std::erase_if(tops_, [&](const auto& t) { return t.get() == top; });
}

Window* Gui::detach(Window* w)
{
    if (!w || !w->parent) {
        return w;
    }
    const SDL_Point pos = w->screenPos();
    auto& siblings = w->parent->children;
    const auto it = std::find_if(siblings.begin(), siblings.end(), [&](const auto& c) { return c.get() == w; });
    if (it == siblings.end()) {
        return w;
    }
    std::unique_ptr<Window> owned = std::move(*it);
    siblings.erase(it);
    owned->parent = nullptr;
    owned->rect.x = pos.x;
    owned->rect.y = pos.y;
    tops_.push_back(std::move(owned));
    return w;
}

void Gui::destroyAll()
{
    slides_.clear();
    tops_.clear();
    hover_ = nullptr;
    pressed_ = nullptr;
    modal_ = nullptr;
}

Slide* Gui::slideIn(Window* window, SDL_Point from, SDL_Point speedIn, SDL_Point speedOut)
{
    auto s = std::make_unique<Slide>();
    s->window = window;
    s->to = {window->rect.x, window->rect.y};
    s->from = from;
    s->speedInXY = speedIn;
    s->speedOutXY = speedOut;
    window->rect.x = from.x;
    window->rect.y = from.y;
    slides_.push_back(std::move(s));
    res_.sound(0x39A); // ShellSlideIn
    return slides_.back().get();
}

void Gui::slideOut(Slide* slide, std::function<void()> onOut)
{
    if (!slide) {
        if (onOut) {
            onOut();
        }
        return;
    }
    slide->out = true;
    slide->done = false;
    slide->onOut = std::move(onOut);
    res_.sound(0x39B); // ShellSlideOut (the screens' leaving, the like)
}

bool Gui::sliding() const
{
    return std::any_of(slides_.begin(), slides_.end(), [](const auto& s) { return !s->done; });
}

void Gui::update(std::uint64_t nowMs)
{
    if (lastStepMs_ == 0) {
        lastStepMs_ = nowMs;
    }
    std::vector<std::function<void()>> finished;
    // Fixed 33 ms steps (30 a second), however often it's called.
    while (nowMs - lastStepMs_ >= 33) {
        lastStepMs_ += 33;
        for (auto& s : slides_) {
            if (s->done) {
                continue;
            }
            const SDL_Point target = s->out ? s->from : s->to;
            const SDL_Point speed = s->out ? s->speedOutXY : s->speedInXY;
            const auto step = [](int& v, int goal, int by) {
                if (by == 0 || v == goal) {
                    v = goal;
                    return;
                }
                v += by;
                if ((by > 0 && v > goal) || (by < 0 && v < goal)) {
                    v = goal;
                }
            };
            step(s->window->rect.x, target.x, speed.x);
            step(s->window->rect.y, target.y, speed.y);
            if (s->window->rect.x == target.x && s->window->rect.y == target.y) {
                s->done = true;
                if (s->out && s->onOut) {
                    finished.push_back(std::move(s->onOut));
                    s->onOut = nullptr;
                }
            }
        }
    }
    // Callbacks last: they may destroy windows (and their slides).
    for (auto& f : finished) {
        f();
    }
}

void Gui::draw(SDL_Renderer* renderer, int width, int height)
{
    Painter p = painter(renderer, width, height);
    // Top-level windows in three passes: BELOW ones, then the rest, then ABOVE ones.
    const auto pass = [&](auto keep) {
        for (auto& t : tops_) {
            if (keep(t->status)) {
                drawWindow(*t, p);
            }
        }
    };
    pass([](std::uint32_t s) { return (s & st::Below) != 0; });
    pass([](std::uint32_t s) { return (s & (st::Below | st::Above)) == 0; });
    pass([](std::uint32_t s) { return (s & st::Above) != 0; });
}

void Gui::drawWindow(Window& w, Painter& p)
{
    // itself, its border, then its children from the newest.
    if (w.hidden()) {
        return;
    }
    if (w.draw) {
        w.draw(w, p);
    } else {
        drawByType(w, p);
    }
    if (w.status & st::Border) {
        drawBorder(w, p);
    }
    for (auto it = w.children.rbegin(); it != w.children.rend(); ++it) {
        drawWindow(**it, p);
    }
}

void Gui::drawBorder(Window& w, Painter& p)
{
    // 20-unit edge pieces round the rectangle 10 out, 10-unit pieces to finish, and
    // the corners. (List boxes and entry fields frame a slightly different rectangle,
    // the plain rectangle here.)
    const SDL_Point pos = w.screenPos();
    const int x = pos.x;
    const int y = pos.y;
    const int width = w.rect.w;
    const int height = w.rect.h;
    const int top = y - 10;
    const int bottom = y - 10 + height;
    int i = x + 10;
    for (; i <= width + x - 30; i += 20) {
        p.image("BorderHorizontal", i, top);
        p.image("BorderHorizontal", i, bottom);
    }
    const int right = width + x - 10;
    if (right - i > 9) {
        p.image("BorderHorizontalShort", i, top);
        p.image("BorderHorizontalShort", i, bottom);
        i += 10;
    }
    if (i < right) {
        // Room for less than a piece: one more short piece, moved back so it ends at the corner (the gap
        // rounded up to even).
        i = i - 10 + ((right - i + 1) & ~1);
        p.image("BorderHorizontalShort", i, top);
        p.image("BorderHorizontalShort", i, bottom);
    }
    const int left = x - 10;
    int j = y + 10;
    for (; j <= y - 30 + height; j += 20) {
        p.image("BorderVertical", left, j);
        p.image("BorderVertical", right, j);
    }
    if (bottom - j > 9) {
        p.image("BorderVerticalShort", left, j);
        p.image("BorderVerticalShort", right, j);
        j += 10;
    }
    if (j < bottom) {
        j = j - 10 + ((bottom - j + 1) & ~1);
        p.image("BorderVerticalShort", left, j);
        p.image("BorderVerticalShort", right, j);
    }
    p.image("BorderCornerUL", left, top);
    p.image("BorderCornerUR", right, top);
    p.image("BorderCornerLL", left, bottom);
    p.image("BorderCornerLR", right, bottom);
}

// The stock look of a window that has no draw function of its own, by its type (the exe's
// own draw functions for each kind of window), in layout pixels.
void Gui::drawByType(Window& w, Painter& p)
{
    const SDL_Point pos = w.screenPos();
    const int x = pos.x;
    const int y = pos.y;
    const int width = w.rect.w;
    const int height = w.rect.h;
    const bool withImage = (w.status & st::Image) != 0;
    const bool on = w.enabled();
    const bool lit = (w.state & kLit) != 0;
    const bool pressed = (w.state & kPressed) != 0;
    const int ix = x + w.imageOffset.x;
    const int iy = y + w.imageOffset.y;

    // Text centred in the window (the buttons' wndButtonDrawNoImg), or in its first `across` units.
    const auto centredText = [&](int across) {
        if (w.text.empty()) {
            return;
        }
        const int tw = p.textWidth(w.text, w.largeFont);
        p.text(w.text, x + across / 2 - tw / 2, y + height / 2 - p.fontHeight(w.largeFont) / 2, w.largeFont,
               w.textColor, across);
    };

    switch (w.type) {
    case WindowType::PushButton:
        // With images: the base (or disabled) image, then the pressed, lit or plain one over it.
        // Without: an outlined box filled by state.
        if (withImage) {
            std::string base = on ? w.backgroundImage : w.disabledImage;
            std::string over = w.enabledImage;
            if (on && pressed) {
                over = w.selectedImage;
            } else if (on && lit) {
                over = w.hiliteImage;
            }
            p.image(base, ix, iy);
            p.image(over, ix, iy);
        } else {
            Color border = w.enabledColor;
            Color fillColor = w.backgroundColor;
            if (!on) {
                fillColor = w.disabledColor;
            } else if (pressed) {
                fillColor = w.selectedColor;
            } else if (lit) {
                border = w.hiliteColor;
            }
            p.outline(x, y, width, height, border);
            p.fill(x + 1, y + 1, width - 2, height - 2, fillColor);
        }
        if (w.dropArrow) {
            // (noxsdl's drop-downs) The game's list-box down arrow at the right end, the gem lit
            // while the mouse is on it; drawn as a small wedge if thing.bin has no such image.
            constexpr int kArrowW = 10, kArrowH = 13;
            const int ax = x + width - kArrowW - 4;
            const int ay = y + (height - kArrowH) / 2;
            const std::string arrow = !on ? "UIScrollDnDis" : lit || pressed ? "UIScrollDnLit" : "UIScrollDn";
            if (imageExists(res_.things, arrow)) {
                p.image(arrow, ax, ay);
            } else {
                const Color c = on && (lit || pressed) ? w.hiliteColor : w.enabledColor;
                for (int row = 0; row < 5; ++row) {
                    p.fill(ax + row, ay + 4 + row, kArrowW - 2 * row, 1, c);
                }
            }
            // The choice centred left of the arrow on one line: in the small font if it's too
            // long for the large one there.
            if (!w.text.empty()) {
                const int across = width - kArrowW - 6;
                const bool large = w.largeFont && p.textWidth(w.text, true) <= across - 4;
                const int tw = p.textWidth(w.text, large);
                p.text(w.text, x + std::max(2, (across - tw) / 2), y + height / 2 - p.fontHeight(large) / 2, large,
                       w.textColor);
            }
            return;
        }
        centredText(width);
        return;

    case WindowType::RadioButton:
    case WindowType::CheckBox: {
        const bool check = w.type == WindowType::CheckBox;
        std::string base = on ? w.backgroundImage : w.disabledImage;
        std::string over = on && lit ? w.hiliteImage : w.enabledImage;
        if (pressed) {
            over = w.selectedImage;
        }
        // Radio buttons at their offset (wndRadioButtonDraw); check boxes 4 in and centred.
        const int bx = check ? ix + 4 : ix;
        const int by = check ? w.imageOffset.y + y + height / 2 - 5 : iy;
        if (withImage) {
            p.image(base, bx, by);
            p.image(over, bx, by);
        } else {
            p.outline(x, y, height, height, lit ? w.hiliteColor : w.enabledColor);
            if (pressed) {
                p.fill(x + 3, y + 3, height - 6, height - 6, w.selectedColor);
            }
        }
        if (!w.text.empty()) {
            const int fh = p.fontHeight(w.largeFont);
            if (check) {
                p.text(w.text, x + 20, y + height / 2 - fh / 2, w.largeFont, w.textColor, width);
            } else if (w.radioValue == 0) {
                p.text(w.text, x + 28, y + (height - fh) / 2, w.largeFont, w.textColor, width);
            } else {
                centredText(width);
            }
        }
        return;
    }

    case WindowType::StaticText: {
        // wndStaticDrawNoImage: the fill (disabled colour when disabled), the border in the enabled (or
        // hilite) colour, then the text, centred or at the left, with a glow in the selected
        // colour one pixel round it. With IMAGE: the background image, then the
        // enabled or the disabled one.
        if (withImage) {
            p.image(w.backgroundImage, ix, iy);
            p.image(on ? w.enabledImage : w.disabledImage, ix, iy);
        } else {
            p.fill(x + 1, y + 1, width - 2, height - 2, on ? w.backgroundColor : w.disabledColor);
            p.outline(x, y, width, height, on && lit ? w.hiliteColor : w.enabledColor);
        }
        const auto* data = std::get_if<nox::gui::StaticTextData>(&w.data);
        if (w.text.empty()) {
            return;
        }
        // The wrapped block centred up and down.
        const bool centre = data && data->center;
        const bool glow = data && data->glow;
        const int tw = p.textWidth(w.text, w.largeFont);
        const int tx = centre ? x + (width - std::min(tw, width)) / 2 : x + 2;
        const int ty = y + height / 2 - p.textHeight(w.text, w.largeFont, width) / 2;
        if (glow) {
            for (const auto& [dx, dy] : {std::pair{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}) {
                p.text(w.text, tx + dx, ty + dy, w.largeFont, w.selectedColor, width);
            }
        }
        p.text(w.text, tx, ty, w.largeFont, w.textColor, width);
        return;
    }

    case WindowType::ScrollListBox: {
        // wndListboxDrawWithImage / wndListboxDrawNoImage: a title line, then the lines; the selected one on the selected
        // colour.
        int top = y;
        int h = height;
        const int fh = p.fontHeight(w.largeFont);
        if (withImage) {
            p.image(on ? w.backgroundImage : w.disabledImage, x, y);
        }
        if (!w.text.empty()) {
            p.text(w.text, x + 1, top, w.largeFont, w.textColor, width);
            top += 1 + fh;
            h -= 1 + fh;
        }
        if (!withImage) {
            p.fill(x, top, width, h, on ? w.backgroundColor : w.disabledColor);
        }
        p.outline(x, top, width, h, on && lit ? w.hiliteColor : w.enabledColor);
        int ly = top;
        // A line too wide for the box wraps there (7 in from its width), as tall as its lines
        // (wndListBoxAddLine's height, wndListboxDrawNoImage's drawStringWrap).
        const int wrapWidth = width - 7;
        // Each line's height first, so a wrapped line takes the rows it needs.
        for (auto& l : w.lines) {
            l.height = lineHeight(w);
            if (l.text.find('\t') == std::string::npos && p.textWidth(l.text, w.largeFont) > wrapWidth) {
                l.height = std::max(l.height, p.textHeight(l.text, w.largeFont, wrapWidth));
            }
        }
        // From the first scrolled-to line, as many as fit in the box.
        for (std::size_t i = static_cast<std::size_t>(std::max(w.scroll, 0)); i < w.lines.size(); ++i) {
            const int step = w.lines[i].height + 1;
            if (ly + step > top + h + 1) {
                break;
            }
            if (static_cast<int>(i) == w.selectedLine || w.lines[i].marked) {
                p.fill(x, ly, width, step, w.selectedColor);
            }
            // (a tab: the rest at the tab stop, draw_setTabWidth's)
            const std::string& line = w.lines[i].text;
            if (const auto tab = line.find('\t'); tab != std::string::npos) {
                p.text(line.substr(0, tab), x + 5, ly + 2, w.largeFont, w.lines[i].color);
                p.text(line.substr(tab + 1), x + 5 + w.tabWidth, ly + 2, w.largeFont, w.lines[i].color);
            } else {
                p.text(line, x + 5, ly + 2, w.largeFont, w.lines[i].color,
                       w.lines[i].height > lineHeight(w) ? wrapWidth : 0);
            }
            ly += step;
        }
        return;
    }

    case WindowType::VertSlider:
    case WindowType::HorzSlider: {
        const bool vertical = w.type == WindowType::VertSlider;
        if (withImage) {
            // The thumb (its enabled / lit image) along the track.
            const auto* sl = std::get_if<nox::gui::SliderData>(&w.data);
            const int range = sl ? std::max(1, sl->max - sl->min) : 1;
            // A vertical one has its maximum at the top (wndScrollBoxProc: the thumb at max - value).
            const int at = sl ? (vertical ? sl->max - w.sliderValue : w.sliderValue - sl->min) : 0;
            const int tx = vertical ? x : x + (width - w.thumbLength) * at / range;
            const int ty = vertical ? y + (height - w.thumbLength) * at / range : y;
            p.image(lit ? w.hiliteImage : w.enabledImage, tx + w.imageOffset.x, ty + w.imageOffset.y);
            return;
        }
        Color fillColor = on ? w.backgroundColor : w.disabledColor;
        Color bar = w.enabledColor;
        if (on && lit) {
            bar = w.selectedColor;
            p.outline(x, y, width, height, w.hiliteColor);
        }
        p.fill(x + 1, y + 1, width - 2, height - 2, fillColor);
        if (vertical) {
            p.fill(x + width / 2 - 1, y + 4, 3, height - 8, bar);
        } else {
            p.fill(x, y + height / 2 - 1, width, 3, bar);
            // (noxsdl's own across slider, Options' mouse speed, has no thumb image: a block
            // where the value is, on the same track the image's would run.)
            if (const auto* sl = std::get_if<nox::gui::SliderData>(&w.data)) {
                const int range = std::max(1, sl->max - sl->min);
                const int tx = x + (width - 10) * (w.sliderValue - sl->min) / range;
                p.fill(tx, y + 1, 10, height - 2, on ? w.hiliteColor : w.enabledColor);
            }
        }
        return;
    }

    case WindowType::EntryField: {
        // wndEditDrawNoImage: its label, then the box and the text in it (stars for a password), 5 in,
        // and a blinking caret after it while it has the focus. ONE_LINE fields drop characters
        // from the front to keep the end in view.
        int bx = x;
        int bw = width;
        const int fh = p.fontHeight(w.largeFont);
        const int ty = y + height / 2 - fh / 2;
        if (!w.text.empty()) {
            const int lw = p.textWidth(w.text, w.largeFont);
            p.text(w.text, x + 2, ty, w.largeFont, w.textColor);
            bx += 6 + lw;
            bw -= 6 + lw;
        }
        const auto* ef = std::get_if<nox::gui::EntryFieldData>(&w.data);
        if (ef && ef->width != 0xFFFF && ef->width > 0 && ef->width < bw) {  // a set box width (0xFFFF: none) puts the box at the right end
            bx = x + width - ef->width;
            bw = ef->width;
        }
        p.fill(bx + 1, y + 1, bw - 2, height - 2, on ? w.backgroundColor : w.disabledColor);
        p.outline(bx, y, bw, height, on && lit ? w.hiliteColor : w.enabledColor);
        std::u16string shown = ef && ef->password ? std::u16string(w.entry.size(), u'*') : w.entry;
        std::string utf8 = nox::strings::toUtf8(shown);
        int tw = p.textWidth(utf8, w.largeFont);
        if ((w.status & st::OneLine) && bw > 9) {
            while (!shown.empty() && tw + 10 > bw) {
                shown.erase(0, 1);
                utf8 = nox::strings::toUtf8(shown);
                tw = p.textWidth(utf8, w.largeFont);
            }
        }
        p.text(utf8, bx + 5, ty, w.largeFont, w.textColor);
        if (focus_ == &w && (SDL_GetTicks() / 267) % 2 == 0) { // every 8 frames at 30 a second
            p.fill(bx + 5 + tw, ty, 2, fh, w.textColor);
        }
        return;
    }

    case WindowType::ProgressBar: {
        // (IMAGE): the background image clipped to `progress` percent of the width;
        // without one that much filled in the enabled colour.
        const int filled = width * std::clamp(w.progress, 0, 100) / 100;
        if (filled <= 0) {
            return;
        }
        if (withImage) {
            SDL_Renderer* r = p.renderer();
            SDL_Rect previous{};
            const bool clipped = SDL_RenderClipEnabled(r);
            SDL_GetRenderClipRect(r, &previous);
            const SDL_FRect area = p.toOutput(x, y, filled, height);
            const SDL_Rect clip{static_cast<int>(std::floor(area.x)), static_cast<int>(std::floor(area.y)),
                                static_cast<int>(std::ceil(area.w)), static_cast<int>(std::ceil(area.h))};
            SDL_SetRenderClipRect(r, &clip);
            p.image(w.backgroundImage, ix, iy);
            SDL_SetRenderClipRect(r, clipped ? &previous : nullptr);
        } else {
            p.fill(x, y, filled, height, w.enabledColor);
        }
        return;
    }

    default:
        // the background image (lit: the hilite image), or the background colour.
        if (withImage) {
            p.image(lit ? w.hiliteImage : w.backgroundImage, ix, iy);
        } else {
            p.fill(x, y, width, height, w.backgroundColor);
        }
        return;
    }
}

Window* Gui::hit(Window& w, int x, int y, bool press)
{
    // A press goes only to ENABLED windows (cursorUpdate pass
    // over the others and what's in them); the mouse's hover finds them too (their tooltips).
    if (w.hidden() || (press && !w.enabled())) {
        return nullptr;
    }
    const SDL_Point pos = w.screenPos();
    const bool inside = x >= pos.x && y >= pos.y && x < pos.x + w.rect.w && y < pos.y + w.rect.h;
    // The last child in the file first: puts each new child at the head of the
    // list and searches it from there (the drawing, walks the
    // same list, so the file's first child is drawn on top yet is the last to get the mouse).
    for (auto it = w.children.rbegin(); it != w.children.rend(); ++it) {
        if (Window* h = hit(**it, x, y, press)) {
            return h;
        }
    }
    if (inside && !(w.status & st::NoInput)) {
        return &w;
    }
    return nullptr;
}

// The window at (x, y) over all the top-level windows: only the modal one's if set, else ABOVE
// ones first, then the rest, then BELOW ones, the newest first in each.
Window* Gui::hitTop(int x, int y, bool press)
{
    if (modal_) {
        return hit(*modal_, x, y, press);
    }
    const auto pass = [&](auto keep) -> Window* {
        for (auto it = tops_.rbegin(); it != tops_.rend(); ++it) {
            if (keep((*it)->status)) {
                if (Window* h = hit(**it, x, y, press)) {
                    return h;
                }
            }
        }
        return nullptr;
    };
    if (Window* h = pass([](std::uint32_t s) { return (s & st::Above) != 0; })) {
        return h;
    }
    if (Window* h = pass([](std::uint32_t s) { return (s & (st::Below | st::Above)) == 0; })) {
        return h;
    }
    return pass([](std::uint32_t s) { return (s & st::Below) != 0; });
}

void Gui::dragSlider(Window& w, SDL_Point at)
{
    // The value under the mouse, as the drawing places the thumb (10 pixels unless set) along the track; the
    // parent hears of a change (0x4009, as wndScrollBoxDraw's input does).
    const auto* sl = std::get_if<nox::gui::SliderData>(&w.data);
    if (!sl) {
        return;
    }
    const SDL_Point pos = w.screenPos();
    const bool vertical = w.type == WindowType::VertSlider;
    const int along = vertical ? at.y - pos.y - w.thumbLength / 2 : at.x - pos.x - w.thumbLength / 2;
    const int track = std::max(1, (vertical ? w.rect.h : w.rect.w) - w.thumbLength);
    const int range = std::max(1, sl->max - sl->min);
    // A vertical one has its maximum at the top (wndScrollBoxProc).
    const int offset = along * range / track;
    const int value = std::clamp(vertical ? sl->max - offset : sl->min + offset, sl->min, sl->max);
    if (value != w.sliderValue) {
        w.sliderValue = value;
        w.notify(SliderChanged);
    }
}

bool Gui::handleEvent(const SDL_Event& event, int width, int height)
{
    const Painter p = painter(nullptr, width, height);
    const auto layoutPoint = [&](float ox, float oy) {
        const SDL_FPoint l = p.toLayout(ox, oy);
        return SDL_Point{static_cast<int>(std::floor(l.x)), static_cast<int>(std::floor(l.y))};
    };
    const auto isButton = [](const Window& w) {
        return w.type == WindowType::PushButton || w.type == WindowType::RadioButton ||
               w.type == WindowType::CheckBox;
    };
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const SDL_Point at = layoutPoint(event.motion.x, event.motion.y);
        // A held slider follows the mouse; otherwise track which window the mouse is on.
        if (pressed_ && pressed_->enabled() &&
            (pressed_->type == WindowType::VertSlider || pressed_->type == WindowType::HorzSlider)) {
            dragSlider(*pressed_, at);
            return true;
        }
        Window* h = hitTop(at.x, at.y, false);
        if (h != hover_) {
            // 0x12 / 0x11 to the widgets that track the mouse.
            if (hover_ && isButton(*hover_) && (hover_->style & sy::MouseTrack)) {
                hover_->state &= ~kLit;
                hover_->notify(Unhover);
                if (hover_->type == WindowType::PushButton) {
                    hover_->state &= ~kPressed;  // dragged off: the release won't click it
                }
            }
            hover_ = h;
            if (hover_ && hover_->enabled() && isButton(*hover_) && (hover_->style & sy::MouseTrack)) {
                hover_->state |= kLit;
                hover_->notify(Hover);
            }
        }
        return h != nullptr;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (event.button.button != SDL_BUTTON_LEFT) {
            return false;
        }
        const SDL_Point at = layoutPoint(event.button.x, event.button.y);
        Window* h = hitTop(at.x, at.y, true);
        pressed_ = h;
        if (h && h->enabled() && h->type == WindowType::PushButton) {
            h->state |= kPressed; // message 5
        }
        if (h && h->enabled() && h->type == WindowType::EntryField) {
            focus_ = h; // an entry field takes the focus when clicked
        }
        if (h && h->enabled() && (h->type == WindowType::VertSlider || h->type == WindowType::HorzSlider)) {
            dragSlider(*h, at); // the thumb jumps to the mouse, and follows it while held
        }
        return h != nullptr;
    }
    case SDL_EVENT_TEXT_INPUT: {
        if (!focus_ || !focus_->enabled() || !event.text.text) {
            return false;
        }
        for (const char16_t c : nox::strings::fromUtf8(event.text.text)) {
            if (c >= 0x20 && focus_->entry.size() < focus_->entryMax) {  // printable characters only, up to the field's limit
                focus_->entry.push_back(c);
            }
        }
        return true;
    }
    case SDL_EVENT_KEY_DOWN: {
        if (!focus_ || !focus_->enabled()) {
            return false;
        }
        if (event.key.key == SDLK_BACKSPACE) {
            if (!focus_->entry.empty()) {
                focus_->entry.pop_back();
            }
            return true;
        }
        if ((event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) && !event.key.repeat &&
            focus_->type == WindowType::EntryField) {
            focus_->notify(EntryEntered); // (wndEditProc: Enter, to the parent)
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.button != SDL_BUTTON_LEFT) {
            return false;
        }
        const SDL_Point at = layoutPoint(event.button.x, event.button.y);
        Window* h = hitTop(at.x, at.y, true);
        // The release goes to the window that took the press; most kinds act only if it is still under the mouse.
        Window* w = pressed_;
        pressed_ = nullptr;
        if (!w || !w->enabled()) {
            return h != nullptr;
        }
        switch (w->type) {
        case WindowType::PushButton:
            if ((w->state & kPressed) && h == w) {
                w->state &= ~kPressed;
                w->notify(Clicked); // wndButtonProc, message 6
            }
            w->state &= ~kPressed;
            break;
        case WindowType::CheckBox:
            if (h == w) {
                w->state ^= kPressed; // (so the parent sees the new state)
                w->notify(Clicked);
            }
            break;
        case WindowType::VertSlider:
        case WindowType::HorzSlider:
            dragSlider(*w, at);
            break;
        case WindowType::RadioButton:
            if (h == w && !(w->state & kPressed)) {
                w->notify(Clicked); // wndRadioButtonProc: the others in its group go off
                if (w->parent) {
                    for (auto& s : w->parent->children) {
                        if (s.get() != w && s->group == w->group) {
                            s->state &= ~kPressed;
                        }
                    }
                }
                w->state |= kPressed;
            }
            break;
        case WindowType::ScrollListBox:
            if (h == w) {
                // The line under the mouse (lines are a font height and a pixel apart, below the
                // title line if there is one).
                const int fh = w->largeFont ? 16 : 10;
                const int top = w->screenPos().y + (w->text.empty() ? 0 : fh + 1);
                int row = std::max(w->scroll, 0);
                // Down the visible lines by their heights to the one under the mouse.
                for (int y = top; row < static_cast<int>(w->lines.size()); ++row) {
                    const auto& l = w->lines[static_cast<std::size_t>(row)];
                    y += (l.height > 0 ? l.height : lineHeight(*w)) + 1;
                    if (at.y < y) {
                        break;
                    }
                }
                if (at.y >= top && row < static_cast<int>(w->lines.size())) {
                    w->selectedLine = row;
                    w->notify(Clicked);
                }
            }
            break;
        default:
            if (h == w) {
                w->notify(Clicked);
            }
            break;
        }
        return h != nullptr;
    }
    default:
        return false;
    }
}

} // namespace noxsdl::gui
