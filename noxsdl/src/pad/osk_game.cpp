// (noxcpp) The game controller's on-screen keyboard in the program (app.hpp's pad/osk_game.cpp group;
// the keyboard itself: pad/osk.hpp).
//
// It opens when the pad gives an entry field the typing focus (A on it: Gui::navigate notes it), when
// the chat line (Chat, TeamChat) or the console opens with the pad the device in use; it closes once
// that's gone (the field left or hidden, the line sent, the console shut), on B, when a real key is
// pressed or the mouse is used (the keyboard's and mouse's again), and when the last pad goes.
//
// What it types goes the way the keyboard's typing does: SDL text events and Backspace / Enter /
// Escape key events pushed into SDL's queue, so the field's own screen, the chat line and the console
// take them through their own handling (the field's limit, its screen's Enter). Its keys say they're
// the pad's (GamepadManager::kPadKeys), so they never hand the device back to the keyboard. SDL's
// text input stays as the screen set it; where the system has a screen keyboard of its own (a Steam
// Deck's), that one is kept shut while ours is used.
//
// While it's up it has every pad press (padEventFirst): the D-pad and the left stick move between its
// keys (held: repeating), A types, X deletes, Y a space, LT held shifts, L3 caps lock, Start enters,
// B closes (the chat line and the console shut with it, as Escape does), LB / RB the quick-chat list
// in a game's chat. Nothing here runs without a pad.
#include "app.hpp"
#include "gui_nav.hpp"
#include "pad/osk.hpp"

#include "noxcore/strings/l10n.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace noxsdl {

namespace {

using pad::Osk;
using pad::OskInput;
using pad::OskKeyKind;
using pad::OskState;
using pad::PadButton;
using Target = OskState::Target;
using nox::gui::Color;

/// A held direction's repeat (as the menus'), and the left stick's press and let-go tilts.
constexpr std::uint64_t kFirstMs = 380;
constexpr std::uint64_t kEveryMs = 110;
constexpr float kStickPress = 0.55f;
constexpr float kStickLetGo = 0.35f;

/// The event type a press the keyboard took becomes: nobody's (registered once).
Uint32 takenEventType()
{
    static const Uint32 type = [] {
        const Uint32 t = SDL_RegisterEvents(1);
        return t != 0 ? t : static_cast<Uint32>(SDL_EVENT_USER) + 0x4E1;
    }();
    return type;
}

std::optional<OskInput> dpadInput(PadButton b)
{
    switch (b) {
    case PadButton::DpadUp: return OskInput::Up;
    case PadButton::DpadDown: return OskInput::Down;
    case PadButton::DpadLeft: return OskInput::Left;
    case PadButton::DpadRight: return OskInput::Right;
    default: return std::nullopt;
    }
}

/// The pad's buttons on the keyboard, by their place.
std::optional<OskInput> buttonInput(PadButton b)
{
    if (const auto d = dpadInput(b)) {
        return d;
    }
    switch (b) {
    case PadButton::South: return OskInput::Press;
    case PadButton::West: return OskInput::Backspace;
    case PadButton::North: return OskInput::Space;
    case PadButton::Start: return OskInput::Enter;
    case PadButton::East: return OskInput::Close;
    case PadButton::LeftShoulder:
    case PadButton::RightShoulder: return OskInput::Pane;
    case PadButton::LeftStick: return OskInput::CapsLock;
    default: return std::nullopt; // (LT: Shift, held; the rest nothing)
    }
}

void pushKey(App& app, SDL_Keycode key, SDL_Scancode scancode)
{
    for (const bool down : {true, false}) {
        SDL_Event e = keyEvent(key, scancode, down);
        e.key.which = pad::GamepadManager::kPadKeys;
        e.key.windowID = app.window ? SDL_GetWindowID(app.window) : 0;
        SDL_PushEvent(&e);
    }
}

void pushText(App& app, std::string_view utf8)
{
    if (utf8.empty()) {
        return;
    }
    OskState& s = app.osk;
    std::string& kept = s.sent[s.sentAt];
    s.sentAt = (s.sentAt + 1) % s.sent.size();
    kept.assign(utf8);
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = app.window ? SDL_GetWindowID(app.window) : 0;
    e.text.text = kept.c_str();
    SDL_PushEvent(&e);
}

void closeOsk(App& app, bool flush);

/// The keyboard's typing, into SDL's queue as the keyboard's.
class Sink final : public pad::OskSink {
public:
    explicit Sink(App& app) : app_(app) {}
    void text(std::string_view utf8) override { pushText(app_, utf8); }
    void backspace() override { pushKey(app_, SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE); }
    void enter() override
    {
        pushKey(app_, SDLK_RETURN, SDL_SCANCODE_RETURN);
        if (app_.osk.target == Target::Field) {
            closeOsk(app_, false); // (a field's Enter: done with it; the chat line goes with its own)
        }
    }
    void close() override
    {
        if (app_.osk.target == Target::Field) {
            closeOsk(app_, true); // (the field keeps what's typed)
            return;
        }
        // The chat line or the console shut with it, as Escape shuts them.
        app_.osk.osk.dropComposing();
        pushKey(app_, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        closeOsk(app_, false);
    }
    void quick(std::size_t index) override
    {
        OskState& s = app_.osk;
        if (index >= s.quick.size() || s.target != Target::Chat || !app_.chat || !app_.chat->isOpen()) {
            return;
        }
        const OskState::QuickLine line = s.quick[index];
        closeOsk(app_, false);
        if (!line.control.empty()) {
            app_.chat->close(app_.window);
            if (app_.live) {
                liveControls(app_, {line.control}); // (Taunt, Laugh, Point: as their keys)
            }
            return;
        }
        app_.chat->sendLine(app_.window, line.text);
    }

private:
    App& app_;
};

void closeOsk(App& app, bool flush)
{
    OskState& s = app.osk;
    if (!s.osk.isOpen()) {
        return;
    }
    if (flush) {
        Sink sink(app);
        s.osk.flush(sink);
    }
    s.osk.close();
    s.target = Target::None;
    s.gui = nullptr;
    s.field = nullptr;
    s.held.reset();
    s.stick.reset();
    SDL_Log("osk: closed");
}

/// What a button is called on the pad in use (its face's label; the bumpers, trigger, stick and
/// Start by the pad's family).
std::string buttonName(const App& app, PadButton b)
{
    switch (app.pads.faceLabel(b)) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return NOX_TR("osk.button.cross", "Cross");
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return NOX_TR("osk.button.circle", "Circle");
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return NOX_TR("osk.button.square", "Square");
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return NOX_TR("osk.button.triangle", "Triangle");
    default: break;
    }
    const SDL_GamepadType t = app.pads.activeType();
    const bool ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    const bool sw = t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                    t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    switch (b) {
    case PadButton::LeftShoulder: return ps ? "L1" : sw ? "L" : "LB";
    case PadButton::RightShoulder: return ps ? "R1" : sw ? "R" : "RB";
    case PadButton::LeftTrigger: return ps ? "L2" : sw ? "ZL" : "LT";
    case PadButton::LeftStick: return "L3";
    case PadButton::Start: return ps ? "Options" : sw ? "+" : "Start";
    default: return std::string(pad::padButtonName(b));
    }
}

/// The texts it draws, in the interface's language (made when it opens, never a frame).
void makeTexts(App& app)
{
    OskState& s = app.osk;
    s.keyNames[static_cast<std::size_t>(OskKeyKind::Shift)] = NOX_TR("osk.key.shift", "Shift");
    s.keyNames[static_cast<std::size_t>(OskKeyKind::Space)] = NOX_TR("osk.key.space", "Space");
    s.keyNames[static_cast<std::size_t>(OskKeyKind::Backspace)] = NOX_TR("osk.key.delete", "Delete");
    s.keyNames[static_cast<std::size_t>(OskKeyKind::Enter)] = NOX_TR("osk.key.enter", "Enter");
    s.keyNames[static_cast<std::size_t>(OskKeyKind::Quick)] = NOX_TR("osk.key.quick", "Quick");
    s.keyNames[static_cast<std::size_t>(OskKeyKind::Close)] = NOX_TR("osk.key.close", "Close");
    const auto hint = [&](PadButton b, const std::string& what) { return buttonName(app, b) + " " + what + "    "; };
    s.hints[0] = hint(PadButton::South, NOX_TR("osk.hint.type", "type")) + hint(PadButton::West, NOX_TR("osk.hint.delete", "delete")) +
                 hint(PadButton::North, NOX_TR("osk.hint.space", "space")) + hint(PadButton::Start, NOX_TR("osk.hint.enter", "enter")) +
                 hint(PadButton::East, NOX_TR("osk.hint.close", "close"));
    s.hints[1] = hint(PadButton::LeftTrigger, NOX_TR("osk.hint.shift", "shift")) + hint(PadButton::LeftStick, NOX_TR("osk.hint.caps", "caps lock"));
    if (!s.quick.empty()) {
        s.hints[1] += buttonName(app, PadButton::LeftShoulder) + " / " + buttonName(app, PadButton::RightShoulder) + " " +
                      NOX_TR("osk.hint.quick", "quick chat");
    }
    s.quickTitle = NOX_TR("osk.quick.title", "Quick chat");
}

/// The quick-chat lines: short phrases said at once, and the game's taunt, laugh and point.
void makeQuickLines(App& app)
{
    OskState& s = app.osk;
    s.quick.clear();
    for (const std::string& line : {NOX_TR("osk.quick.hi", "Hi!"), NOX_TR("osk.quick.gg", "gg"), NOX_TR("osk.quick.thanks", "Thanks!"),
                                    NOX_TR("osk.quick.help", "Help!"), NOX_TR("osk.quick.follow", "Follow me!"),
                                    NOX_TR("osk.quick.wait", "Wait!"), NOX_TR("osk.quick.ok", "OK"), NOX_TR("osk.quick.sorry", "Sorry!"),
                                    NOX_TR("osk.quick.luck", "Good luck!"), NOX_TR("osk.quick.nice", "Nice one!")}) {
        s.quick.push_back({line, {}});
    }
    s.quick.push_back({NOX_TR("osk.quick.taunt", "(Taunt)"), "Taunt"});
    s.quick.push_back({NOX_TR("osk.quick.laugh", "(Laugh)"), "Laugh"});
    s.quick.push_back({NOX_TR("osk.quick.point", "(Point)"), "Point"});
}

void openOsk(App& app, Target target, gui::Gui* g = nullptr, gui::Window* field = nullptr)
{
    OskState& s = app.osk;
    s.target = target;
    s.gui = g;
    s.field = field;
    if (target == Target::Chat) {
        makeQuickLines(app);
    } else {
        s.quick.clear();
    }
    makeTexts(app);
    if (!s.res) {
        s.res.emplace(guiResources(app));
    }
    const pad::OskScript script = pad::oskScriptFor(nox::l10n::language());
    s.osk.open(script, s.quick.size());
    s.held.reset();
    s.stick.reset();
    // A system keyboard of its own (a Steam Deck's) stays shut: ours is the pad's.
    if (app.window && SDL_HasScreenKeyboardSupport()) {
        SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
        if (SDL_ScreenKeyboardShown(app.window)) {
            SDL_StopTextInput(app.window);
            SDL_StartTextInput(app.window);
        }
    }
    SDL_Log("osk: open on the %s (%s)", target == Target::Field ? "entry field" : target == Target::Chat ? "chat line" : "console",
            script == pad::OskScript::Hangul ? "Hangul" : script == pad::OskScript::Cyrillic ? "Cyrillic" : "Latin");
}

/// Whether what it types into is still there to type into.
bool targetAlive(const App& app)
{
    const OskState& s = app.osk;
    const bool console = app.console && app.console->isOpen();
    switch (s.target) {
    case Target::Field:
        return s.gui && gui::navOwner() == s.gui && s.gui->focus() == s.field && s.field && gui::shownAndEnabled(*s.field);
    case Target::Chat:
        return app.live && app.chat && app.chat->isOpen() && !console; // (the console over it has the keys)
    case Target::Console:
        return console;
    case Target::None:
        break;
    }
    return false;
}

void input(App& app, OskInput in)
{
    Sink sink(app);
    app.osk.osk.input(in, app.pads.isDown(PadButton::LeftTrigger), sink);
}

/// The left stick's way (the larger of its two leans), with hysteresis.
std::optional<OskInput> stickWay(const App& app, std::optional<OskInput> held)
{
    SDL_Gamepad* g = app.pads.active();
    if (!g) {
        return std::nullopt;
    }
    const pad::PadSettings& st = app.pads.settings();
    const pad::StickVec v = pad::radialDeadZone({pad::axisUnit(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)),
                                                 pad::axisUnit(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY))},
                                                st.deadZone, st.outerDeadZone);
    if (v.length() < (held ? kStickLetGo : kStickPress)) {
        return std::nullopt;
    }
    if (std::fabs(v.x) > std::fabs(v.y)) {
        return v.x > 0 ? OskInput::Right : OskInput::Left;
    }
    return v.y > 0 ? OskInput::Down : OskInput::Up;
}

} // namespace

bool oskOpen(const App& app)
{
    return app.osk.osk.isOpen();
}

bool oskEvent(App& app, SDL_Event& event, const std::optional<pad::PadEdge>& edge)
{
    OskState& s = app.osk;
    // A real key, or the input method's typing: the keyboard's again (the key goes on to the field).
    if ((event.type == SDL_EVENT_KEY_DOWN && event.key.which != pad::GamepadManager::kPadKeys &&
         event.key.which != gui::kPadKeyboard) ||
        (event.type == SDL_EVENT_TEXT_EDITING && event.edit.text && *event.edit.text)) {
        closeOsk(app, true);
        return false;
    }
    if (!edge) {
        return false;
    }
    if (!edge->down) {
        if (s.held == edge->button) {
            s.held.reset();
        }
        return false; // (its release goes on: nothing was held for it)
    }
    if (const auto in = buttonInput(edge->button)) {
        input(app, *in);
        if (dpadInput(edge->button) && s.osk.isOpen()) {
            s.held = edge->button;
            s.heldInput = *in;
            s.nextMs = SDL_GetTicks() + kFirstMs;
        }
    }
    event.type = takenEventType();
    return true;
}

void oskFrame(App& app)
{
    OskState& s = app.osk;
    if (!app.pads.connected()) {
        closeOsk(app, false);
        s.chatWas = s.consoleWas = false;
        return;
    }
    // Gone: what it typed into, the pad (the mouse or a key used), or a movie over it.
    if (s.osk.isOpen()) {
        if (!targetAlive(app)) {
            closeOsk(app, false);
        } else if (!app.pads.padInUse() || (app.movie && app.movie->active())) {
            closeOsk(app, true);
        }
    }
    // Opened with the pad: a field the pad's A went into, the chat line, the console.
    const bool chat = app.live && app.chat && app.chat->isOpen();
    const bool console = app.console && app.console->isOpen();
    const auto typing = gui::takePadTyping();
    if (!s.osk.isOpen() && app.pads.padInUse()) {
        if (console && !s.consoleWas) {
            openOsk(app, Target::Console);
        } else if (chat && !console && !s.chatWas) {
            openOsk(app, Target::Chat);
        } else if (typing && typing->gui && gui::navOwner() == typing->gui && typing->gui->focus() == typing->field) {
            openOsk(app, Target::Field, typing->gui, typing->field);
        }
    }
    s.chatWas = chat;
    s.consoleWas = console;
    if (!s.osk.isOpen()) {
        return;
    }
    // A held D-pad direction, and the left stick, repeat.
    const std::uint64_t now = SDL_GetTicks();
    if (s.held) {
        if (!app.pads.isDown(*s.held)) {
            s.held.reset();
        } else if (now >= s.nextMs) {
            input(app, s.heldInput);
            s.nextMs = now + kEveryMs;
        }
        s.stick.reset();
        return;
    }
    const std::optional<OskInput> way = stickWay(app, s.stick);
    if (way != s.stick) {
        s.stick = way;
        if (way) {
            input(app, *way);
            s.nextMs = now + kFirstMs;
        }
    } else if (way && now >= s.nextMs) {
        input(app, *way);
        s.nextMs = now + kEveryMs;
    }
}

namespace {

// The look: the game's black dialogs with their border, gold text (dlg.wnd's 240 180 42), the pad's
// gold for what the pad is on.
constexpr Color kText{240, 180, 42};
constexpr Color kTextLit{255, 236, 160};
constexpr Color kDim{150, 128, 96};
constexpr Color kKeyFill{26, 20, 12};
constexpr Color kKeyEdge{112, 84, 34};
constexpr Color kKeyOn{88, 64, 22};
constexpr Color kPadGold{250, 204, 82};
constexpr Color kBoxFill{10, 8, 6};

constexpr int kHalf = 12;     ///< a half key's width
constexpr int kKeyH = 22;     ///< a key's height (and a row's)
constexpr int kGap = 2;       ///< between keys
constexpr int kPad = 8;       ///< inside the frame
constexpr int kLineH = 20;    ///< the line being typed
constexpr int kHintH = 11;    ///< a line of hints
constexpr int kQuickW = 124;  ///< the quick-chat list
constexpr int kQuickLineH = 11;

/// Text centred in a box.
void centred(gui::Painter& p, const std::string& t, int x, int y, int w, int h, bool large, const Color& c)
{
    const int tw = p.textWidth(t, large);
    p.text(t, x + (w - tw) / 2, y + (h - p.fontHeight(large)) / 2, large, c);
}

void drawKey(App& app, gui::Painter& p, const pad::OskKey& k, bool on, int x, int y, int w)
{
    const OskState& s = app.osk;
    const bool shiftLit = k.kind == OskKeyKind::Shift && (s.osk.shiftOnce() || app.pads.isDown(PadButton::LeftTrigger) || s.osk.capsLock());
    p.fill(x, y, w, kKeyH, on ? kKeyOn : kKeyFill);
    p.outline(x, y, w, kKeyH, on ? kPadGold : shiftLit ? kText : kKeyEdge);
    if (on) {
        p.outline(x + 1, y + 1, w - 2, kKeyH - 2, kPadGold);
    }
    const Color& c = on ? kTextLit : kText;
    switch (k.kind) {
    case OskKeyKind::Char:
        centred(p, std::string(s.osk.typed(k, app.pads.isDown(PadButton::LeftTrigger))), x, y, w, kKeyH, true, c);
        return;
    case OskKeyKind::Switch: {
        // (the other script by its own name)
        const bool toLatin = s.osk.script() != pad::OskScript::Latin;
        const char* name = toLatin ? "ABC" : s.osk.native() == pad::OskScript::Hangul ? "한글" : "РУС";
        centred(p, name, x, y, w, kKeyH, false, c);
        return;
    }
    default: {
        std::string name = s.keyNames[static_cast<std::size_t>(k.kind)];
        if (k.kind == OskKeyKind::Shift && s.osk.capsLock()) {
            name += " *";
        }
        centred(p, name, x, y, w, kKeyH, false, c);
        return;
    }
    }
}

/// The line being typed, its end in view (stars for a password), the syllable being composed after
/// it, and a caret.
void drawLine(App& app, gui::Painter& p, int x, int y, int w)
{
    const OskState& s = app.osk;
    std::string typed;
    std::string label;
    switch (s.target) {
    case Target::Field: {
        const auto* ef = std::get_if<nox::gui::EntryFieldData>(&s.field->data);
        typed = ef && ef->password ? std::string(s.field->entry.size(), '*') : nox::strings::toUtf8(s.field->entry);
        label = s.field->text;
        break;
    }
    case Target::Chat:
        typed = app.chat->text();
        break;
    case Target::Console:
        typed = app.console->input();
        label = ">";
        break;
    case Target::None:
        break;
    }
    p.fill(x, y, w, kLineH, kBoxFill);
    p.outline(x, y, w, kLineH, kKeyEdge);
    int tx = x + 5;
    if (!label.empty()) {
        p.text(label, tx, y + (kLineH - p.fontHeight(false)) / 2, false, kDim);
        tx += p.textWidth(label, false) + 6;
    }
    const std::string pre = s.osk.preedit();
    const int room = x + w - 8 - tx - p.textWidth(pre, true);
    // From the end back as much as fits.
    std::size_t start = 0;
    while (start < typed.size() && p.textWidth(typed.substr(start), true) > room) {
        do {
            ++start;
        } while (start < typed.size() && (static_cast<unsigned char>(typed[start]) & 0xC0) == 0x80);
    }
    const std::string shown = typed.substr(start);
    const int ty = y + (kLineH - p.fontHeight(true)) / 2;
    p.text(shown, tx, ty, true, kTextLit);
    tx += p.textWidth(shown, true);
    if (!pre.empty()) {
        const int pw = p.textWidth(pre, true);
        p.fill(tx, ty, pw + 2, p.fontHeight(true), kKeyOn);
        p.text(pre, tx + 1, ty, true, kTextLit);
        tx += pw + 2;
    }
    if ((SDL_GetTicks() / 267) % 2 == 0) {
        p.fill(tx + 1, ty, 2, p.fontHeight(true), kTextLit);
    }
}

} // namespace

void drawOsk(App& app)
{
    OskState& s = app.osk;
    if (!s.osk.isOpen() || !s.res || !app.renderer) {
        return;
    }
    gui::Painter p(app.renderer, *s.res, app.camera.viewWidth, app.camera.viewHeight);
    const pad::OskLayout& l = s.osk.layout();
    const int keysW = pad::oskWidth(l) * kHalf;
    const int rows = static_cast<int>(l.rows.size());
    const bool quick = !s.quick.empty();
    const int w = kPad + keysW + kPad + (quick ? kQuickW + kPad : 0);
    const int h = kPad + kLineH + 6 + rows * kKeyH + 4 + 2 * kHintH + kPad;
    const int x = (640 - w) / 2;
    const int y = 480 - h - 14; // (low on the screen: the game, the console's lines above it)
    p.fillAlpha(x, y, w, h, SDL_Color{0, 0, 0, 225});
    gui::drawFrame(p, x, y, w, h);

    drawLine(app, p, x + kPad, y + kPad, keysW);
    const int top = y + kPad + kLineH + 6;
    for (int r = 0; r < rows; ++r) {
        const auto& row = l.rows[static_cast<std::size_t>(r)];
        for (int c = 0; c < static_cast<int>(row.size()); ++c) {
            const auto [a, b] = pad::oskKeySpan(l, {r, c});
            const bool on = !s.osk.quickPane() && s.osk.pos() == pad::OskPos{r, c};
            drawKey(app, p, row[static_cast<std::size_t>(c)], on, x + kPad + a * kHalf, top + r * kKeyH, (b - a) * kHalf - kGap);
        }
    }
    const int hy = top + rows * kKeyH + 4;
    p.text(s.hints[0], x + kPad, hy, false, kDim);
    p.text(s.hints[1], x + kPad, hy + kHintH, false, kDim);

    if (quick) {
        const int qx = x + kPad + keysW + kPad;
        const int qw = kQuickW;
        p.fill(qx, y + kPad, qw, h - 2 * kPad, kBoxFill);
        p.outline(qx, y + kPad, qw, h - 2 * kPad, kKeyEdge);
        p.text(s.quickTitle, qx + 5, y + kPad + 3, false, kText);
        const int ly = y + kPad + 3 + kHintH + 2;
        for (std::size_t i = 0; i < s.quick.size(); ++i) {
            const int at = ly + static_cast<int>(i) * kQuickLineH;
            const bool on = s.osk.quickPane() && s.osk.quickIndex() == i;
            if (on) {
                p.fill(qx + 2, at - 1, qw - 4, kQuickLineH, kKeyOn);
                p.outline(qx + 2, at - 1, qw - 4, kQuickLineH, kPadGold);
            }
            p.text(s.quick[i].text, qx + 6, at, false, on ? kTextLit : s.quick[i].control.empty() ? kText : kDim);
        }
    }
}

} // namespace noxsdl
