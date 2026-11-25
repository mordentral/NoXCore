// (noxcpp) The window system's navigation focus for the game controller (gui_nav.hpp).
#include "gui_nav.hpp"

#include "gui.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iterator>

namespace noxsdl::gui {

namespace st = nox::gui::status;
using nox::gui::WindowType;

namespace {

/// The coordinator's state (one program, one pad layer).
struct Coordinator {
    bool connected = false;
    bool inUse = false;
    Gui* owner = nullptr;   ///< last frame's claim
    Gui* claimed = nullptr; ///< this frame's, so far
};
Coordinator coordinator;

bool isButton(const Window& w)
{
    return w.type == WindowType::PushButton || w.type == WindowType::RadioButton || w.type == WindowType::CheckBox;
}

bool within(const Window* w, const Window* ancestor)
{
    for (; w; w = w->parent) {
        if (w == ancestor) {
            return true;
        }
    }
    return false;
}

void collect(Window& w, const Scene& scene, std::vector<Window*>& out)
{
    if (w.hidden() || !w.enabled() || w.nav == Window::Nav::Off) {
        return; // (a click passes over these and everything in them; Off: the pad does too)
    }
    if (navKind(w) && w.rect.w > 0 && w.rect.h > 0) {
        // Reached by a click at its middle: nothing over it there (a drop-down's list, a window
        // on top) takes the click instead.
        const SDL_Rect r = screenRect(w);
        const Window* h = hitScene(scene, r.x + r.w / 2, r.y + r.h / 2, true);
        if (h == &w || within(h, &w) || (h && within(&w, h)) || (!h && w.nav == Window::Nav::On)) {
            out.push_back(&w);
        }
    }
    for (auto& c : w.children) {
        collect(*c, scene, out);
    }
}

/// It and every window it's in shown.
bool shown(const Window& w)
{
    for (const Window* p = &w; p; p = p->parent) {
        if (p->hidden()) {
            return false;
        }
    }
    return true;
}

const Window* topOf(const Window* w)
{
    while (w->parent) {
        w = w->parent;
    }
    return w;
}

int lineHeight(const Window& w, const Window::Line& l)
{
    return (l.height > 0 ? l.height : listLineHeight(w)) + 1;
}

/// Reading order: the higher first, then the one further left.
bool readsBefore(const Window* a, const Window* b)
{
    const SDL_Rect ra = screenRect(*a);
    const SDL_Rect rb = screenRect(*b);
    if (ra.y != rb.y) {
        return ra.y < rb.y;
    }
    return ra.x < rb.x;
}

} // namespace

// ------------------------------------------------------------------------------- nav events

Uint32 navEventType()
{
    static const Uint32 type = [] {
        const Uint32 t = SDL_RegisterEvents(1);
        return t != 0 ? t : static_cast<Uint32>(SDL_EVENT_USER) + 0x4E0; // (none left: a fixed one in the user range)
    }();
    return type;
}

SDL_Event navEvent(NavInput input)
{
    SDL_Event e{};
    e.type = navEventType();
    e.user.code = static_cast<Sint32>(input);
    return e;
}

std::optional<NavInput> navInput(const SDL_Event& event)
{
    if (event.type != navEventType() || event.user.code < 0 || event.user.code > static_cast<Sint32>(NavInput::TabNext)) {
        return std::nullopt;
    }
    return static_cast<NavInput>(event.user.code);
}

// ------------------------------------------------------------------------------ coordinator

void navFrame(bool connected, bool inUse)
{
    coordinator.connected = connected;
    coordinator.inUse = connected && inUse;
    coordinator.owner = connected ? coordinator.claimed : nullptr;
    coordinator.claimed = nullptr;
}

Gui* navOwner()
{
    return coordinator.owner;
}

bool navConnected()
{
    return coordinator.connected;
}

bool navPadInUse()
{
    return coordinator.inUse;
}

void navClaim(Gui* gui)
{
    coordinator.claimed = gui;
}

void navForget(const Gui* gui)
{
    if (coordinator.owner == gui) {
        coordinator.owner = nullptr;
    }
    if (coordinator.claimed == gui) {
        coordinator.claimed = nullptr;
    }
}

// ------------------------------------------------------------------------------- geometry

SDL_Rect screenRect(const Window& w)
{
    const SDL_Point p = w.screenPos();
    return {p.x, p.y, w.rect.w, w.rect.h};
}

int pickInDirection(const SDL_Rect& from, std::span<const SDL_Rect> candidates, NavInput dir)
{
    const bool horizontal = dir == NavInput::Left || dir == NavInput::Right;
    const bool forward = dir == NavInput::Right || dir == NavInput::Down;
    if (!horizontal && dir != NavInput::Up && dir != NavInput::Down) {
        return -1;
    }
    // Along the way pressed and across it, as [start, end) spans and middles (doubled: whole numbers).
    const auto along = [&](const SDL_Rect& r) { return horizontal ? std::pair{r.x, r.x + r.w} : std::pair{r.y, r.y + r.h}; };
    const auto across = [&](const SDL_Rect& r) { return horizontal ? std::pair{r.y, r.y + r.h} : std::pair{r.x, r.x + r.w}; };
    const auto [fa0, fa1] = along(from);
    const auto [fc0, fc1] = across(from);
    const int fromMid = fa0 + fa1;
    const int fromAcross = fc0 + fc1;
    int best = -1;
    long long bestScore = LLONG_MAX;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const SDL_Rect& c = candidates[i];
        const auto [ca0, ca1] = along(c);
        const auto [cc0, cc1] = across(c);
        const int mid = ca0 + ca1;
        if (forward ? mid <= fromMid : mid >= fromMid) {
            continue; // (not that way)
        }
        const int gap = std::max(0, forward ? ca0 - fa1 : fa0 - ca1);
        const int gapAcross = std::max(0, std::max(cc0 - fc1, fc0 - cc1));
        const int offAcross = std::abs((cc0 + cc1) - fromAcross) / 2;
        const long long score = static_cast<long long>(gap) * 4 + static_cast<long long>(gapAcross) * 30 + offAcross;
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// -------------------------------------------------------------------------------- hit tests

Window* hitWindow(Window& w, int x, int y, bool press)
{
    // A press goes only to ENABLED windows (and what's in them); the hover finds the others too
    // (their tooltips). The last child in the file first (the drawing walks the same list, so the
    // file's first child is drawn on top yet is the last to get the mouse).
    if (w.hidden() || (press && !w.enabled())) {
        return nullptr;
    }
    const SDL_Point pos = w.screenPos();
    const bool inside = x >= pos.x && y >= pos.y && x < pos.x + w.rect.w && y < pos.y + w.rect.h;
    for (auto it = w.children.rbegin(); it != w.children.rend(); ++it) {
        if (Window* h = hitWindow(**it, x, y, press)) {
            return h;
        }
    }
    if (inside && !(w.status & st::NoInput)) {
        return &w;
    }
    return nullptr;
}

Window* hitScene(const Scene& scene, int x, int y, bool press)
{
    if (scene.modal) {
        return hitWindow(*scene.modal, x, y, press);
    }
    const auto pass = [&](auto keep) -> Window* {
        for (auto it = scene.tops.rbegin(); it != scene.tops.rend(); ++it) {
            if (keep((*it)->status)) {
                if (Window* h = hitWindow(**it, x, y, press)) {
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

// ------------------------------------------------------------------------------- candidates

bool navKind(const Window& w)
{
    if (w.nav != Window::Nav::Auto) {
        return w.nav == Window::Nav::On;
    }
    if (w.padInput) {
        return true;
    }
    if (w.status & st::NoInput) {
        return false;
    }
    switch (w.type) {
    case WindowType::ScrollListBox:
        // (a list box the game uses as a frame round other windows, with no lines of its own, is
        // the windows in it)
        return w.children.empty() || !w.lines.empty();
    case WindowType::PushButton:
    case WindowType::RadioButton:
    case WindowType::CheckBox:
    case WindowType::HorzSlider:
    case WindowType::EntryField:
        return true;
    default:
        return false;
    }
}

bool shownAndEnabled(const Window& w)
{
    return w.enabled() && shown(w);
}

std::vector<Window*> navCandidates(const Scene& scene)
{
    std::vector<Window*> out;
    if (scene.modal) {
        collect(*scene.modal, scene, out);
        return out;
    }
    for (const auto& t : scene.tops) {
        collect(*t, scene, out);
    }
    return out;
}

// ---------------------------------------------------------------------- what a window does

void hoverTo(Window*& hover, Window* to)
{
    if (to == hover) {
        return;
    }
    // 0x12 / 0x11 to the widgets that track the mouse.
    if (hover && isButton(*hover) && (hover->style & nox::gui::style::MouseTrack)) {
        hover->state &= ~kLit;
        hover->notify(Unhover);
        if (hover->type == WindowType::PushButton) {
            hover->state &= ~kPressed; // dragged off: the release won't click it
        }
    }
    hover = to;
    if (hover && hover->enabled() && isButton(*hover) && (hover->style & nox::gui::style::MouseTrack)) {
        hover->state |= kLit;
        hover->notify(Hover);
    }
}

void activate(Window& w, Window*& typing)
{
    if (!w.enabled()) {
        return;
    }
    switch (w.type) {
    case WindowType::PushButton:
        w.state &= ~kPressed;
        w.notify(Clicked); // wndButtonProc, message 6
        break;
    case WindowType::CheckBox:
        w.state ^= kPressed; // (so the parent sees the new state)
        w.notify(Clicked);
        break;
    case WindowType::RadioButton:
        if (!(w.state & kPressed)) {
            w.notify(Clicked); // wndRadioButtonProc: the others in its group go off
            if (w.parent) {
                for (auto& s : w.parent->children) {
                    if (s.get() != &w && s->group == w.group) {
                        s->state &= ~kPressed;
                    }
                }
            }
            w.state |= kPressed;
        }
        break;
    case WindowType::ScrollListBox:
        if (w.selectedLine >= 0 && w.selectedLine < static_cast<int>(w.lines.size())) {
            w.notify(Clicked);
        }
        break;
    case WindowType::EntryField:
        typing = &w; // (an entry field takes the focus when clicked)
        w.notify(Clicked);
        break;
    default:
        w.notify(Clicked);
        break;
    }
}

bool stepSlider(Window& w, int dir)
{
    const auto* sl = std::get_if<nox::gui::SliderData>(&w.data);
    if (!sl || dir == 0) {
        return false;
    }
    const int lo = std::min(sl->min, sl->max);
    const int hi = std::max(sl->min, sl->max);
    const int step = std::max(1, (hi - lo) / 20);
    const int value = std::clamp(w.sliderValue + (dir > 0 ? step : -step), lo, hi);
    if (value == w.sliderValue) {
        return false;
    }
    w.sliderValue = value;
    w.notify(SliderChanged);
    return true;
}

bool keepLineInView(Window& w)
{
    const int count = static_cast<int>(w.lines.size());
    const int sel = w.selectedLine;
    if (sel < 0 || sel >= count) {
        return false;
    }
    const int before = w.scroll;
    if (sel < w.scroll) {
        w.scroll = sel;
    } else {
        // Its lines from the first shown down to the selected one fit the box (below its title).
        const int room = w.rect.h - (w.text.empty() ? 0 : fontLine(w) + 1);
        const auto used = [&] {
            int h = 0;
            for (int i = std::max(w.scroll, 0); i <= sel; ++i) {
                h += lineHeight(w, w.lines[static_cast<std::size_t>(i)]);
            }
            return h;
        };
        while (w.scroll < sel && used() > room) {
            ++w.scroll;
        }
    }
    return w.scroll != before;
}

bool stepList(Window& w, int dir)
{
    const int count = static_cast<int>(w.lines.size());
    if (count == 0 || dir == 0) {
        return false;
    }
    int line = w.selectedLine;
    if (dir > 0) {
        if (line + 1 >= count) {
            return false;
        }
        line = std::max(line + 1, 0);
    } else {
        if (line <= 0) {
            return false;
        }
        line = std::min(line - 1, count - 1);
    }
    w.selectedLine = line;
    if (keepLineInView(w)) {
        w.notify(PadScrolled);
    }
    if (w.navLineClicks) {
        w.notify(Clicked); // (as a click on the line)
    }
    return true;
}

// -------------------------------------------------------------------------------- Navigator

void Navigator::focusOn(Window* w, Window*& hover)
{
    if (!hover && focus_ && focus_ != w) {
        hover = focus_; // (the pointing let go when another top-level window went: the focus still lit)
    }
    focus_ = w;
    lost_.reset();
    hoverTo(hover, w);
}

bool Navigator::settle(const std::vector<Window*>& candidates, Window*& hover, bool passive)
{
    const auto isCandidate = [&](const Window* w) { return w && std::ranges::find(candidates, w) != candidates.end(); };
    if (isCandidate(focus_)) {
        return true;
    }
    if (passive && focus_ && !focus_->enabled() && shown(*focus_)) {
        return true; // (greyed for now, its screen still up: kept till a press moves it)
    }
    if (focus_) {
        if (shownAndEnabled(*focus_)) {
            // Still there but out of reach (a popup over it, a modal window): back to it after.
            if (restore_.empty() || restore_.back() != focus_) {
                restore_.push_back(focus_);
            }
        } else {
            lost_ = screenRect(*focus_);
        }
        focus_ = nullptr;
    }
    if (candidates.empty()) {
        return false;
    }
    Window* pick = nullptr;
    // The latest kept focus that's in reach again (the ones kept after it go with it).
    for (auto it = restore_.rbegin(); it != restore_.rend(); ++it) {
        if (isCandidate(*it)) {
            pick = *it;
            restore_.erase(std::prev(it.base()), restore_.end());
            break;
        }
    }
    if (!pick && isCandidate(default_)) {
        pick = default_;
    }
    if (!pick && lost_) {
        // The one nearest where it was.
        const SDL_Rect& l = *lost_;
        long long bestD = LLONG_MAX;
        for (Window* c : candidates) {
            const SDL_Rect r = screenRect(*c);
            const long long dx = (r.x + r.w / 2) - (l.x + l.w / 2);
            const long long dy = (r.y + r.h / 2) - (l.y + l.h / 2);
            if (dx * dx + dy * dy < bestD) {
                bestD = dx * dx + dy * dy;
                pick = c;
            }
        }
    }
    if (!pick) {
        pick = *std::ranges::min_element(candidates, readsBefore);
    }
    default_ = nullptr;
    focusOn(pick, hover);
    return true;
}

bool Navigator::input(NavInput in, const Scene& scene, Window*& hover, Window*& typing, const std::function<bool(int)>& tabs)
{
    if (in == NavInput::TabPrevious || in == NavInput::TabNext) {
        return tabs && tabs(in == NavInput::TabNext ? 1 : -1);
    }
    const std::vector<Window*> candidates = navCandidates(scene);
    const bool had = focus_ && std::ranges::find(candidates, focus_) != candidates.end();
    if (!settle(candidates, hover)) {
        return false;
    }
    if (!had) {
        return true; // (the first press shows where the focus is)
    }
    Window& f = *focus_;
    if (f.padInput && f.padInput(f, in)) {
        return true;
    }
    if (in == NavInput::Accept) {
        activate(f, typing);
        return true;
    }
    // In place: a slider steps, a list's line moves, till its end.
    if (f.type == WindowType::HorzSlider && (in == NavInput::Left || in == NavInput::Right)) {
        stepSlider(f, in == NavInput::Right ? 1 : -1);
        return true;
    }
    if (f.type == WindowType::VertSlider && (in == NavInput::Up || in == NavInput::Down)) {
        stepSlider(f, in == NavInput::Up ? 1 : -1); // (its maximum at the top)
        return true;
    }
    if (f.type == WindowType::ScrollListBox && (in == NavInput::Up || in == NavInput::Down) &&
        stepList(f, in == NavInput::Down ? 1 : -1)) {
        return true;
    }
    // The nearest that way: from a window kept apart (Window::navApart) its own first, else the ones
    // not kept apart first; then any.
    const Window* top = topOf(&f);
    const auto nearest = [&](auto keep) -> Window* {
        std::vector<SDL_Rect> rects;
        std::vector<Window*> others;
        for (Window* c : candidates) {
            if (c != focus_ && keep(*c)) {
                rects.push_back(screenRect(*c));
                others.push_back(c);
            }
        }
        const int next = pickInDirection(screenRect(f), rects, in);
        return next >= 0 ? others[static_cast<std::size_t>(next)] : nullptr;
    };
    Window* next = top->navApart ? nearest([&](const Window& c) { return topOf(&c) == top; })
                                 : nearest([&](const Window& c) { return !topOf(&c)->navApart; });
    if (!next) {
        next = nearest([](const Window&) { return true; });
    }
    if (next) {
        focusOn(next, hover);
    }
    return true; // (nothing that way: still the menu's press)
}

void Navigator::forget(const Window* top)
{
    if (within(focus_, top)) {
        focus_ = nullptr;
        lost_.reset();
    }
    if (within(default_, top)) {
        default_ = nullptr;
    }
    std::erase_if(restore_, [&](const Window* w) { return within(w, top); });
}

void Navigator::clear()
{
    focus_ = nullptr;
    default_ = nullptr;
    restore_.clear();
    lost_.reset();
}

} // namespace noxsdl::gui
