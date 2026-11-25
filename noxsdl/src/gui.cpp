// The window system's windows, painter, drawing and input (gui.hpp describes the design).
#include "gui.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::gui {

namespace st = nox::gui::status;
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

} // namespace

// (The windows themselves, and their fonts' lines: gui_window.cpp.)

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
    // (By the db's own index: one kept here by the db's address could outlive the db and answer
    // for another made at the same address.)
    const nox::things::ImageDef* found = things.findImage(name);
    if (!found) {
        return std::nullopt;
    }
    const nox::things::ImageDef& def = *found;
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
    const auto lines = font(large)->wrap(utf8, pixelSize(large), static_cast<float>(wrapWidth) * scale_);
    return lines.empty() ? std::string() : lines.front();
}

int Painter::wrappedWidth(const std::string& utf8, bool large, int wrapWidth)
{
    if (!res_.font || utf8.empty()) {
        return 0;
    }
    float widest = 0;
    for (const auto& line : font(large)->wrap(utf8, pixelSize(large), static_cast<float>(wrapWidth) * scale_)) {
        widest = std::max(widest, font(large)->measure(line, pixelSize(large)));
    }
    return static_cast<int>(std::ceil(widest / scale_));
}

std::vector<std::string> Painter::centredLines(const std::string& utf8, bool large, int wrapWidth)
{
    if (!res_.font) {
        return {utf8};
    }
    return noxsdl::centredLines(utf8, static_cast<float>(wrapWidth) * scale_, font(large)->advances(pixelSize(large)));
}

void Painter::image(const std::string& name, int x, int y)
{
    const auto ref = namedImage(res_.things, name);
    drawSprite(ref ? res_.sprites.get(*ref) : nullptr, x, y);
}

void Painter::image(const std::string& name, int x, int y, bool dim)
{
    const auto ref = namedImage(res_.things, name);
    const Sprite* s = ref ? res_.sprites.get(*ref) : nullptr;
    if (!dim || !s || !s->texture) {
        drawSprite(s, x, y);
        return;
    }
    // (the texture's colour scaled down for this draw, then put back: an atlas page others share)
    Uint8 r = 255, g = 255, b = 255;
    SDL_GetTextureColorMod(s->texture, &r, &g, &b);
    SDL_SetTextureColorMod(s->texture, static_cast<Uint8>(r * 2 / 5), static_cast<Uint8>(g * 2 / 5), static_cast<Uint8>(b * 2 / 5));
    drawSprite(s, x, y);
    SDL_SetTextureColorMod(s->texture, r, g, b);
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
    // The TTF drawn so its line is as tall as the game's font at this scale (a Nox-style font: its
    // rows at this scale, Font::lineSize).
    if (!res_.font) {
        return 10;
    }
    return font(large)->lineSize(large ? 16.0f * largeScale_ : 10.0f, scale_, 6);
}

int Painter::textWidth(const std::string& utf8, bool large)
{
    if (!res_.font || utf8.empty()) {
        return 0;
    }
    return static_cast<int>(std::ceil(font(large)->measure(utf8, pixelSize(large)) / scale_));
}

int Painter::textHeight(const std::string& utf8, bool large, int wrapWidth)
{
    if (!res_.font || utf8.empty()) {
        return fontHeight(large);
    }
    const auto lines = wrapWidth > 0 ? font(large)->wrap(utf8, pixelSize(large), static_cast<float>(wrapWidth) * scale_).size()
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
        lines = font(large)->wrap(utf8, size, static_cast<float>(wrapWidth) * scale_);
    } else {
        lines.push_back(utf8);
    }
    float oy = offsetY_ + static_cast<float>(y) * scale_;
    for (const auto& line : lines) {
        font(large)->draw(line, offsetX_ + static_cast<float>(x) * scale_, oy, size, sdl(c));
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

Window* Gui::load(const std::filesystem::path& file, Proc proc)
{
    return load(nox::gui::loadWindowFile(file), std::move(proc));
}

Window* Gui::load(const nox::gui::WindowDef& def, Proc proc)
{
    tops_.push_back(buildWindowTree(def, nullptr, res_.strings, proc));
    return tops_.back().get();
}

// Destroys a top-level window: its slides go, hover and press are cleared, and so are the
// modal, the focus and the pad's focus if they were in it.
void Gui::destroy(Window* top)
{
    nav_.forget(top);
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
    nav_.clear(); // ((noxcpp) the pad's focus with them)
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
    if (navConnected()) {
        drawNav(p); // ((noxcpp) the game controller's focus: nothing without a pad)
    }
}

void Gui::drawNav(Painter& p)
{
    if (!padNav_) {
        return;
    }
    const std::vector<Window*> candidates = navCandidates(scene());
    if (candidates.empty()) {
        return;
    }
    navClaim(this);
    if (navOwner() != this || !navPadInUse()) {
        return;
    }
    // Settled once the screen has: the first focus and where it comes back to are picked where the
    // windows are, not on the way in.
    if (!sliding()) {
        nav_.settle(candidates, hover_, true);
    }
    const Window* f = nav_.focus();
    if (!f || std::ranges::find(candidates, f) == candidates.end()) {
        return;
    }
    const SDL_Rect r = screenRect(*f);
    p.outline(r.x - 3, r.y - 3, r.w + 6, r.h + 6, Color{24, 16, 4});
    p.outline(r.x - 2, r.y - 2, r.w + 4, r.h + 4, Color{250, 204, 82});
    p.outline(r.x - 1, r.y - 1, r.w + 2, r.h + 2, Color{250, 204, 82});
}

void Gui::padFocus(Window* w)
{
    if (w && navPadInUse()) {
        nav_.focusOn(w, hover_);
    }
}

bool Gui::stepTabs(std::span<Window* const> tabs, int step)
{
    const int n = static_cast<int>(tabs.size());
    int on = -1;
    for (int i = 0; i < n; ++i) {
        if (tabs[static_cast<std::size_t>(i)] && isChecked(*tabs[static_cast<std::size_t>(i)])) {
            on = i;
            break;
        }
    }
    const int start = on >= 0 ? on : (step > 0 ? -1 : n); // (none on: the first, or the last)
    for (int k = 1; k <= n; ++k) {
        const int i = ((start + (step > 0 ? k : -k)) % n + n) % n;
        Window* t = tabs[static_cast<std::size_t>(i)];
        if (i == on || !t || !shownAndEnabled(*t)) {
            continue;
        }
        nav_.focusOn(t, hover_);
        activate(*t, focus_);
        return true;
    }
    return false;
}

bool Gui::navigate(NavInput input)
{
    if (sliding()) {
        return true; // (no clicks reach a screen while it slides: no pad either)
    }
    return nav_.input(input, scene(), hover_, focus_, tabs_);
}

void Gui::drawWindow(Window& w, Painter& p)
{
    // itself, its border, then its children from the newest.
    if (w.hidden()) {
        return;
    }
    // (its large font at its own share of the size while it draws)
    const float largeScale = p.largeScale();
    p.setLargeScale(w.largeScale);
    if (w.draw) {
        w.draw(w, p);
    } else {
        drawByType(w, p);
    }
    p.setLargeScale(largeScale);
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
const Window::TextFit& Gui::fitText(Window& w, Painter& p, int width, int height)
{
    using Mode = Window::TextFit::Mode;
    Window::TextFit& f = w.fit;
    Font* const big = p.font(w.largeFont);
    Font* const small = p.font(false);
    if (!big || !small) {
        f.mode = Mode::AsDrawn;
        return f;
    }
    const int bigSize = p.pixelSize(w.largeFont);
    const int smallSize = p.pixelSize(false);
    if (f.text == w.text && f.width == width && f.height == height && f.scale == p.scale() && f.large == w.largeFont &&
        f.largeFace == big && f.smallFace == small && f.largeSet == big->facesSet() && f.smallSet == small->facesSet() &&
        f.largeSize == bigSize && f.smallSize == smallSize) {
        return f; // (as worked out last time: nothing changed)
    }
    f.text = w.text;
    f.width = width;
    f.height = height;
    f.scale = p.scale();
    f.large = w.largeFont;
    f.largeFace = big;
    f.smallFace = small;
    f.largeSet = big->facesSet();
    f.smallSet = small->facesSet();
    f.largeSize = bigSize;
    f.smallSize = smallSize;
    f.lines.clear();
    // As the game draws it when that fits: one line within the width, or (a box with room for
    // more lines) its lines within the width and the height.
    const int lineH = p.fontHeight(w.largeFont);
    const int smallH = p.fontHeight(false);
    if (w.text.empty() || p.textWidth(w.text, w.largeFont) <= width) {
        f.mode = Mode::AsDrawn;
        return f;
    }
    const int lines = p.textHeight(w.text, w.largeFont, width) / std::max(lineH, 1);
    if (lines > 1 && lines * lineH <= height && p.wrappedWidth(w.text, w.largeFont, width) <= width) {
        f.mode = Mode::AsDrawn;
        return f;
    }
    // Else the small font: on one line when it fits; wrapped when the box has room for its lines;
    // else the lines the box has room for.
    if (w.largeFont && p.textWidth(w.text, false) <= width) {
        f.mode = Mode::SmallLine;
        return f;
    }
    f.lines = small->wrap(w.text, smallSize, static_cast<float>(width) * p.scale());
    const auto rows = static_cast<std::size_t>(std::max(1, height / std::max(smallH, 1)));
    f.mode = f.lines.size() <= rows ? Mode::SmallWrapped : Mode::SmallClipped;
    if (f.lines.size() > rows) {
        f.lines.resize(rows);
    }
    return f;
}

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
    // (noxcpp) Greyed out (Window::dimDisabled, disabled): the texts and outlines dim, the images
    // darkened. Otherwise the stock colours, whatever the state.
    const bool dim = !on && w.dimDisabled;
    const Color kDimText{112, 106, 96};
    const Color kDimEdge{70, 66, 60};
    const Color textColor = dim ? kDimText : w.textColor;
    const Color edgeColor = dim && !w.enabledColor.transparent ? kDimEdge : w.enabledColor; // (none stays none)

    // (noxcpp) A text that doesn't fit its box as the game draws it (fitText): the small font on
    // one line, or its lines, the block centred up and down in the box; from `left`, or centred in
    // `across` units from it.
    const auto fallbackText = [&](const Window::TextFit& f, int left, int across, bool centred) {
        using Mode = Window::TextFit::Mode;
        const int fh = p.fontHeight(false);
        if (f.mode == Mode::SmallLine) {
            const int tw = centred ? p.textWidth(w.text, false) : 0;
            p.text(w.text, centred ? left + across / 2 - tw / 2 : left, y + height / 2 - fh / 2, false, textColor);
            return;
        }
        int ty = y + std::max(0, (height - static_cast<int>(f.lines.size()) * fh) / 2);
        for (const std::string& line : f.lines) {
            const int tw = centred ? p.textWidth(line, false) : 0;
            p.text(line, centred ? left + across / 2 - tw / 2 : left, ty, false, textColor);
            ty += fh;
        }
    };
    // Text centred in the window (the buttons' wndButtonDrawNoImg), or in its first `across` units.
    const auto centredText = [&](int across) {
        if (w.text.empty()) {
            return;
        }
        if (const auto& f = fitText(w, p, across, height); f.mode != Window::TextFit::Mode::AsDrawn) {
            fallbackText(f, x, across, true);
            return;
        }
        const int tw = p.textWidth(w.text, w.largeFont);
        p.text(w.text, x + across / 2 - tw / 2, y + height / 2 - p.fontHeight(w.largeFont) / 2, w.largeFont,
               textColor, across);
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
            p.image(base, ix, iy, dim);
            p.image(over, ix, iy, dim);
        } else {
            Color border = edgeColor;
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
                       textColor);
            }
            return;
        }
        // (noxsdl's) A plain button one line high (a drop-down's choices): its text on one line, in
        // the small font if it's too long for the large one, never wrapped.
        if (!withImage && !w.text.empty() && height < 2 * p.fontHeight(w.largeFont)) {
            const bool large = w.largeFont && p.textWidth(w.text, true) <= width - 4;
            const int tw = p.textWidth(w.text, large);
            p.text(w.text, x + width / 2 - tw / 2, y + height / 2 - p.fontHeight(large) / 2, large, textColor);
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
            p.image(base, bx, by, dim);
            p.image(over, bx, by, dim);
        } else {
            p.outline(x, y, height, height, lit && !dim ? w.hiliteColor : edgeColor);
            if (pressed) {
                p.fill(x + 3, y + 3, height - 6, height - 6, dim ? kDimEdge : w.selectedColor);
            }
        }
        if (!w.text.empty()) {
            const int fh = p.fontHeight(w.largeFont);
            if (check) {
                // (noxcpp: one that doesn't fit the box as the game draws it, fitText's way)
                if (const auto& f = fitText(w, p, width, height); f.mode != Window::TextFit::Mode::AsDrawn) {
                    fallbackText(f, x + 20, width, false);
                } else {
                    p.text(w.text, x + 20, y + height / 2 - fh / 2, w.largeFont, textColor, width);
                }
            } else if (w.radioValue == 0 && w.labelLeft > 0) {
                const int room = width - w.labelLeft;
                if (const auto& f = fitText(w, p, room, height); f.mode != Window::TextFit::Mode::AsDrawn) {
                    fallbackText(f, x + w.labelLeft, room, true); // (noxcpp: as a check box's)
                    return;
                }
                const int tw = std::min(p.textWidth(w.text, w.largeFont), room);
                p.text(w.text, x + w.labelLeft + (room - tw) / 2, y + (height - fh) / 2, w.largeFont, textColor, room);
            } else if (w.radioValue == 0) {
                if (const auto& f = fitText(w, p, width, height); f.mode != Window::TextFit::Mode::AsDrawn) {
                    fallbackText(f, x + 28, width, false); // (noxcpp: as a check box's)
                } else {
                    p.text(w.text, x + 28, y + (height - fh) / 2, w.largeFont, textColor, width);
                }
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
            // (greyed: a label keeps its own fill, only its words dimmed)
            p.fill(x + 1, y + 1, width - 2, height - 2, on || dim ? w.backgroundColor : w.disabledColor);
            p.outline(x, y, width, height, on && lit ? w.hiliteColor : edgeColor);
        }
        const auto* data = std::get_if<nox::gui::StaticTextData>(&w.data);
        if (w.text.empty()) {
            return;
        }
        if (w.oneLine) {
            // (noxsdl's labels) One line, centred up and down: in the small font if it's too long
            // for the large one (as wide as the label: it starts 2 in, and the panel leaves 2 after).
            const bool large = w.largeFont && p.textWidth(w.text, true) <= width;
            const int tw = p.textWidth(w.text, large);
            const int tx = data && data->center ? x + (width - std::min(tw, width)) / 2 : x + 2;
            p.text(w.text, tx, y + height / 2 - p.fontHeight(large) / 2, large, textColor);
            return;
        }
        // The wrapped block centred up and down.
        const bool centre = data && data->center;
        const bool glow = data && data->glow;
        // (noxcpp: one that doesn't fit the box as the game draws it, fitText's way)
        if (const auto& f = fitText(w, p, width, height); f.mode != Window::TextFit::Mode::AsDrawn) {
            fallbackText(f, centre ? x : x + 2, width, centre);
            return;
        }
        const auto drawText = [&](const std::string& t, int tx, int ty) {
            if (glow && !dim) {
                for (const auto& [dx, dy] : {std::pair{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}) {
                    p.text(t, tx + dx, ty + dy, w.largeFont, w.selectedColor, width);
                }
            }
            p.text(t, tx, ty, w.largeFont, textColor, width);
        };
        const int textH = p.textHeight(w.text, w.largeFont, width);
        int ty = y + height / 2 - textH / 2;
        if (centre && textH > p.fontHeight(w.largeFont)) {
            // Centred and more than a line: each line centred on its own, a pixel between them
            // (the block's top from the plain wrap's height, as the exe works it out).
            for (const auto& line : p.centredLines(w.text, w.largeFont, width)) {
                drawText(line, x + (width - p.wrappedWidth(line, w.largeFont, width)) / 2, ty);
                ty += p.textHeight(line, w.largeFont, width) + 1;
            }
            return;
        }
        const int tw = p.textWidth(w.text, w.largeFont);
        drawText(w.text, centre ? x + (width - std::min(tw, width)) / 2 : x + 2, ty);
        return;
    }

    case WindowType::ScrollListBox: {
        // wndListboxDrawWithImage / wndListboxDrawNoImage: a title line, then the lines; the selected one on the selected
        // colour.
        int top = y;
        int h = height;
        const int fh = p.fontHeight(w.largeFont);
        if (withImage) {
            p.image(on ? w.backgroundImage : w.disabledImage, x, y, dim);
        }
        if (!w.text.empty()) {
            p.text(w.text, x + 1, top, w.largeFont, textColor, width);
            top += 1 + fh;
            h -= 1 + fh;
        }
        if (!withImage) {
            p.fill(x, top, width, h, on ? w.backgroundColor : w.disabledColor);
        }
        p.outline(x, top, width, h, on && lit ? w.hiliteColor : edgeColor);
        int ly = top;
        // A line too wide for the box wraps there (7 in from its width), as tall as its lines
        // (wndListBoxAddLine's height, wndListboxDrawNoImage's drawStringWrap).
        const int wrapWidth = width - 7;
        // Each line's height first, so a wrapped line takes the rows it needs.
        for (auto& l : w.lines) {
            l.height = listLineHeight(w);
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
                p.fill(x, ly, width, step, dim ? kDimEdge : w.selectedColor);
            }
            // (a tab: the rest at the tab stop, draw_setTabWidth's)
            const std::string& line = w.lines[i].text;
            const Color lineColor = dim ? kDimText : w.lines[i].color;
            if (const auto tab = line.find('\t'); tab != std::string::npos) {
                p.text(line.substr(0, tab), x + 5, ly + 2, w.largeFont, lineColor);
                p.text(line.substr(tab + 1), x + 5 + w.tabWidth, ly + 2, w.largeFont, lineColor);
            } else {
                p.text(line, x + 5, ly + 2, w.largeFont, lineColor,
                       w.lines[i].height > listLineHeight(w) ? wrapWidth : 0);
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
            p.image(lit ? w.hiliteImage : w.enabledImage, tx + w.imageOffset.x, ty + w.imageOffset.y, dim);
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
            p.text(w.text, x + 2, ty, w.largeFont, textColor);
            bx += 6 + lw;
            bw -= 6 + lw;
        }
        const auto* ef = std::get_if<nox::gui::EntryFieldData>(&w.data);
        if (ef && ef->width != 0xFFFF && ef->width > 0 && ef->width < bw) {  // a set box width (0xFFFF: none) puts the box at the right end
            bx = x + width - ef->width;
            bw = ef->width;
        }
        p.fill(bx + 1, y + 1, bw - 2, height - 2, on ? w.backgroundColor : w.disabledColor);
        p.outline(bx, y, bw, height, on && lit ? w.hiliteColor : edgeColor);
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
        p.text(utf8, bx + 5, ty, w.largeFont, textColor);
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

// The window at (x, y) over all the top-level windows: only the modal one's if set, else ABOVE
// ones first, then the rest, then BELOW ones, the newest first in each (gui_nav.cpp's hitScene,
// which the pad's focus searches too; a press only ENABLED ones).
Window* Gui::hitTop(int x, int y, bool press)
{
    return hitScene(scene(), x, y, press);
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
    // (noxcpp) The game controller's menu input: the Gui that has the pad takes it (gui_nav.hpp).
    if (const auto in = navInput(event)) {
        if (!padNav_ || navOwner() != this) {
            return false;
        }
        return navigate(*in);
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const SDL_Point at = layoutPoint(event.motion.x, event.motion.y);
        // A held slider follows the mouse; otherwise track which window the mouse is on
        // (0x12 / 0x11 to the widgets that track the mouse: hoverTo).
        if (pressed_ && pressed_->enabled() &&
            (pressed_->type == WindowType::VertSlider || pressed_->type == WindowType::HorzSlider)) {
            dragSlider(*pressed_, at);
            return true;
        }
        Window* h = hitTop(at.x, at.y, false);
        hoverTo(hover_, h);
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
            // Printable characters only, one fewer than the field's limit as the game's edit box
            // (its last place is the terminator: 8 for the login's 9, 9 for the nick's 10).
            if (c >= 0x20 && focus_->entry.size() + 1 < focus_->entryMax) {
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
        // What a click does is activate()'s (gui_nav.cpp), the pad's A's too.
        switch (w->type) {
        case WindowType::PushButton:
            if ((w->state & kPressed) && h == w) {
                activate(*w, focus_); // wndButtonProc, message 6
            }
            w->state &= ~kPressed;
            break;
        case WindowType::VertSlider:
        case WindowType::HorzSlider:
            dragSlider(*w, at);
            break;
        case WindowType::ScrollListBox:
            if (h == w) {
                // The line under the mouse (lines are a font height and a pixel apart, below the
                // title line if there is one).
                const int fh = fontLine(*w);
                const int top = w->screenPos().y + (w->text.empty() ? 0 : fh + 1);
                int row = std::max(w->scroll, 0);
                // Down the visible lines by their heights to the one under the mouse.
                for (int y = top; row < static_cast<int>(w->lines.size()); ++row) {
                    const auto& l = w->lines[static_cast<std::size_t>(row)];
                    y += (l.height > 0 ? l.height : listLineHeight(*w)) + 1;
                    if (at.y < y) {
                        break;
                    }
                }
                if (at.y >= top && row < static_cast<int>(w->lines.size())) {
                    w->selectedLine = row;
                    activate(*w, focus_); // (Clicked on that line)
                }
            }
            break;
        default:
            // Check boxes flip, radio buttons go on, entry fields keep the focus their press gave.
            if (h == w) {
                activate(*w, focus_);
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
