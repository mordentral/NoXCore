// (noxcpp) The game controller in a game (pad_play.hpp).
#include "pad/pad_play.hpp"

#include "cursor_pick.hpp"
#include "friend_dot.hpp"
#include "pad/pad_panels.hpp"
#include "pointer_map.hpp"

#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/object_class.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

namespace noxsdl {

namespace {

using pad::StickVec;
using pad::WorldPoint;
namespace oc = nox::things::ObjectClass;

constexpr float kNearReach = 130;                       ///< things a hand may take, gathered within this
constexpr float kWantReach = pad::kTargetReach * 1.5f;  ///< creatures, gathered within this
constexpr std::size_t kMaxSeen = 256;
constexpr std::uint32_t kUseableKey = 0x80000000u;      ///< an interact option's key for a map object

SDL_FPoint toScreen(const App& app, WorldPoint p)
{
    return {(p.x - app.camera.left()) * app.camera.zoom, (p.y - app.camera.top()) * app.camera.zoom};
}

/// The pointer (mousePos) on world point `p`, kept on the view.
void setPointer(App& app, WorldPoint p)
{
    const SDL_FPoint s = toScreen(app, p);
    app.padPlay.hasPointer = true;
    app.padPlay.pointerX = std::clamp(s.x, 0.0f, std::max(static_cast<float>(app.camera.viewWidth) - 1, 0.0f));
    app.padPlay.pointerY = std::clamp(s.y, 0.0f, std::max(static_cast<float>(app.camera.viewHeight) - 1, 0.0f));
}

/// The way our drawable faces (its AnimDir: a 3 x 3 grid without the centre), for the aim before
/// the stick has leaned.
StickVec ownFacingWay(const App& app)
{
    const auto& client = app.live->client();
    if (const auto* me = client.world().drawable(client.netCode()); me && me->direction != 4 && me->direction <= 8) {
        const StickVec v{static_cast<float>(me->direction % 3) - 1.0f, static_cast<float>(me->direction / 3) - 1.0f};
        const float l = v.length();
        if (l > 0) {
            return {v.x / l, v.y / l};
        }
    }
    return {1, 0};
}

const pad::AimCandidate* enemyById(const App& app, std::optional<std::uint16_t> id)
{
    if (!id) {
        return nullptr;
    }
    for (const auto& c : app.padPlay.enemies) {
        if (c.id == *id) {
            return &c;
        }
    }
    return nullptr;
}

/// The attack or a slot button held (the lock's hold).
bool lockHeld(const App& app)
{
    static const int kAction = InputBindings::control("Action");
    static const std::array<int, 5> kSlots = {InputBindings::control("InvokeSlot1"), InputBindings::control("InvokeSlot2"),
                                              InputBindings::control("InvokeSlot3"), InputBindings::control("InvokeSlot4"),
                                              InputBindings::control("InvokeSlot5")};
    if (!app.input) {
        return false;
    }
    if (app.input->held(kAction)) {
        return true;
    }
    return std::ranges::any_of(kSlots, [&](int c) { return app.input->held(c); });
}

std::uint8_t facingTo(WorldPoint from, WorldPoint to)
{
    return pick::facing(from.x, from.y, to.x, to.y, std::nullopt);
}

WorldPoint ahead(WorldPoint from, StickVec dir, float by)
{
    return {from.x + dir.x * by, from.y + dir.y * by};
}

// ------------------------------------------------------------------ the free cursor's mouse

/// A made-up mouse event at the free cursor (in window points, as SDL's are: the event's way
/// through SDL_AppEvent maps it into the game's pixels as the mouse's), the pad's mouse id.
void pushFreeMouse(App& app, Uint32 type, Uint8 button, float dx = 0, float dy = 0)
{
    float x = app.padPlay.freeX;
    float y = app.padPlay.freeY;
    pointermap::toWindow(x, y, app.pixelDensity, pointermap::Area{app.gameArea.x, app.gameArea.y, app.gameArea.w, app.gameArea.h},
                         app.letterboxed);
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.which = pad::GamepadManager::kPadMouse;
        e.motion.x = x;
        e.motion.y = y;
        e.motion.xrel = dx;
        e.motion.yrel = dy;
        e.motion.state = (app.padPlay.freeButtons & 1 ? SDL_BUTTON_LMASK : 0u) | (app.padPlay.freeButtons & 2 ? SDL_BUTTON_RMASK : 0u);
    } else {
        e.button.which = pad::GamepadManager::kPadMouse;
        e.button.button = button;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
    }
    SDL_PushEvent(&e);
}

void releaseFreeButtons(App& app)
{
    auto& s = app.padPlay;
    if (s.freeButtons & 1) {
        s.freeButtons &= ~1u;
        pushFreeMouse(app, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    }
    if (s.freeButtons & 2) {
        s.freeButtons &= ~2u;
        pushFreeMouse(app, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT);
    }
}

/// The free cursor moved by the right stick (its motion sent on as the mouse's).
void moveFreeCursor(App& app, StickVec right, float seconds)
{
    auto& s = app.padPlay;
    const float scale = static_cast<float>(app.camera.viewHeight) / 480.0f;
    const StickVec step = pad::freeCursorStep(right, seconds, app.padConfig.settings.sensitivity, scale, s.freeHeld);
    if (step.x != 0 || step.y != 0) {
        const float x = std::clamp(s.freeX + step.x, 0.0f, std::max(static_cast<float>(app.camera.viewWidth) - 1, 0.0f));
        const float y = std::clamp(s.freeY + step.y, 0.0f, std::max(static_cast<float>(app.camera.viewHeight) - 1, 0.0f));
        const float dx = x - s.freeX;
        const float dy = y - s.freeY;
        s.freeX = x;
        s.freeY = y;
        if (dx != 0 || dy != 0) {
            pushFreeMouse(app, SDL_EVENT_MOUSE_MOTION, 0, dx, dy);
        }
    }
    s.hasPointer = true;
    s.pointerX = s.freeX;
    s.pointerY = s.freeY;
}

// ------------------------------------------------------------------------- interact

float extentRing(const TypeArt* art)
{
    if (!art) {
        return 6;
    }
    return art->extentBox ? std::floor((art->extentWidth + art->extentHeight) / 2) + 6
           : art->extentRadius > 0 ? std::floor(art->extentRadius) + 6
                                   : 6;
}

/// An interact pick's key: a drawable's net code, or a map object's extent with kUseableKey.
std::optional<std::uint32_t> interactKey(const std::optional<pad::PlayState::Interact>& it)
{
    if (!it) {
        return std::nullopt;
    }
    return it->drawable ? std::uint32_t{*it->drawable} : kUseableKey | it->extent;
}

bool takes(CursorType c)
{
    return c == CursorType::Pickup || c == CursorType::Caution || c == CursorType::Talk || c == CursorType::Trade || c == CursorType::Use;
}

/// What A takes this frame (app.padPlay.interact): of what's near us, what the mouse's hands
/// would take there (handCursor), the best in front (pad::bestInteract).
void pickInteract(App& app, WorldPoint from, StickVec dir)
{
    auto& s = app.padPlay;
    s.interact.reset();
    const auto& client = app.live->client();
    if (!playing(client) || pick::worldSkipped(worldGate(app))) {
        return;
    }
    static std::vector<pad::InteractOption> options;
    static std::vector<pad::PlayState::Interact> what;
    options.clear();
    what.clear();
    for (const auto& n : s.near) {
        const auto hand = handCursor(app, n.netCode, nullptr);
        if (!hand || !takes(*hand)) {
            continue;
        }
        const auto* d = client.world().drawable(n.netCode);
        const TypeArt* a = d ? app.art->get(d->typeId) : nullptr;
        const bool talk = *hand == CursorType::Talk || *hand == CursorType::Trade;
        options.push_back({n.netCode, n.at, talk ? 125.0f : 75.0f});
        what.push_back({static_cast<std::uint8_t>(*hand), n.netCode, 0, n.at, extentRing(a)});
    }
    if (app.view) {
        const SightMap* sight = app.lineOfSight.value_or(true) && app.sight.polygon().size() >= 3 ? &app.sight : nullptr;
        app.view->useablesNear(from.x, from.y, 80, sight, [&](const MapView::Useable& u) {
            const auto hand = handCursor(app, std::nullopt, &u);
            if (hand && *hand == CursorType::Use) {
                options.push_back({kUseableKey | u.extent, {u.x, u.y}, 75.0f});
                what.push_back({static_cast<std::uint8_t>(*hand), std::nullopt, u.extent, {u.x, u.y}, extentRing(u.art)});
            }
        });
    }
    if (options.empty()) {
        return;
    }
    const bool recent = SDL_GetTicks() < s.triedUntilMs;
    const std::span<const std::uint32_t> skip = recent ? std::span<const std::uint32_t>(s.tried.data(), s.triedCount) : std::span<const std::uint32_t>{};
    if (const auto best = pad::bestInteract(options, from, dir, skip)) {
        s.interact = what[*best];
    }
}

void interact(App& app)
{
    auto& s = app.padPlay;
    if (!s.interact) {
        return;
    }
    const auto it = *s.interact;
    // A pile's things in turn: what was just taken is left out of the next pick a moment.
    const std::uint32_t key = *interactKey(s.interact);
    const std::uint64_t now = SDL_GetTicks();
    if (now >= s.triedUntilMs) {
        s.triedCount = 0;
    }
    if (s.triedCount == s.tried.size()) {
        std::copy(s.tried.begin() + 1, s.tried.end(), s.tried.begin());
        --s.triedCount;
    }
    s.tried[s.triedCount++] = key;
    s.triedUntilMs = now + 1500;
    if (app.scriptPad) {
        SDL_Log("pad: interact %u with hand %u", key, static_cast<unsigned>(it.cursor));
    }
    switch (static_cast<CursorType>(it.cursor)) {
    case CursorType::Pickup:
    case CursorType::Caution:
        if (it.drawable) {
            tryPickup(app, *it.drawable);
        }
        break;
    case CursorType::Talk:
    case CursorType::Trade:
        if (it.drawable) {
            talkOrTrade(app, static_cast<CursorType>(it.cursor) == CursorType::Talk, *it.drawable);
        }
        break;
    case CursorType::Use:
        useWithHand(app, it.drawable ? std::nullopt : std::optional<std::uint32_t>(it.extent), it.drawable);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------- casts

/// How long a cast's aim is held: its chant (a phoneme every three frames) and a little for the
/// server's word that it began.
std::uint64_t castHoldMs(const App& app, std::uint32_t spell)
{
    const auto* def = spell != 0 ? app.things.findSpell(spell) : nullptr;
    return 400 + (def ? def->phonemes.size() : 0) * 100;
}

/// The cursor put on a cast's aim and sent at once, ahead of the cast; held there through its chant.
void aimCast(App& app, pad::SpellAim kind, std::uint32_t spell)
{
    auto& s = app.padPlay;
    auto& client = app.live->client();
    const auto at = client.position();
    if (!at) {
        return;
    }
    const WorldPoint from{static_cast<float>(at->first), static_cast<float>(at->second)};
    const auto target = kind == pad::SpellAim::Self ? std::nullopt : s.targetAt;
    const auto point = pad::castPoint(kind, target, s.reticle, from, s.dir);
    if (!point) {
        return;
    }
    const std::optional<std::uint16_t> sprite = target ? s.target : std::nullopt;
    client.setCursor(static_cast<int>(point->x), static_cast<int>(point->y));
    client.setCursorSprite(sprite);
    client.face(facingTo(from, *point));
    client.flushCursor();
    setPointer(app, *point);
    s.castAt = point;
    s.castTarget = sprite;
    s.castUntilMs = SDL_GetTicks() + castHoldMs(app, spell);
    if (app.scriptPad) {
        SDL_Log("pad: cast %u aimed at %.0f, %.0f (target %d)", spell, point->x, point->y, sprite ? static_cast<int>(*sprite) : -1);
    }
}

/// A slot's aim: its ability's, or its spell's by the spell's flags and the slot's on-self flag.
std::optional<std::pair<pad::SpellAim, std::uint32_t>> slotAim(const App& app, std::size_t i)
{
    if (i >= 5) {
        return std::nullopt;
    }
    const auto slot = app.bars.current()[i];
    if (isWarrior(app)) {
        return std::pair{pad::abilityAim(slot.id), 0u};
    }
    if (nox::client::isTaggedAbility(slot.id)) {
        return std::pair{pad::abilityAim(nox::client::untagged(slot.id)), 0u};
    }
    if (slot.id == 0) {
        return std::nullopt;
    }
    return std::pair{pad::spellAim(nox::client::spellFlags(app.things, slot.id), slot.target), slot.id};
}

/// A spell waiting for its target (app.armed) fired at the aim, as the target cursor's click
/// off the GUI does (leftMouse).
void fireArmed(App& app)
{
    auto& s = app.padPlay;
    const auto a = *app.armed;
    const pad::SpellAim kind = a.trap || a.ability ? pad::SpellAim::Point : pad::spellAim(nox::client::spellFlags(app.things, a.id), false);
    aimCast(app, kind, a.ability || a.trap ? 0u : a.id);
    if (!s.castAt) {
        return;
    }
    app.armed.reset();
    auto& client = app.live->client();
    if (a.trap) {
        buildTrap(app); // where the cursor is
    } else if (a.ability) {
        client.useAbility(static_cast<std::uint8_t>(a.id));
    } else {
        const std::array<std::uint32_t, 1> spells{a.id};
        client.castSpells(spells, false);
        app.lastTargetFlag = false;
        noteCast(app, a.id);
    }
}

bool isSlotControl(const std::string& c)
{
    return c.starts_with("InvokeSlot") && c.size() == 11 && c[10] >= '1' && c[10] <= '5';
}

} // namespace

// ------------------------------------------------------------------------------ the frame

void padPlayIdle(App& app)
{
    auto& s = app.padPlay;
    if (s.freeButtons) {
        releaseFreeButtons(app);
    }
    s.dropAim();
    s.collecting = false;
    s.enemies.clear();
    s.near.clear();
}

PadSteer padPlaySteer(App& app, float fromX, float fromY)
{
    auto& s = app.padPlay;
    auto& client = app.live->client();
    const auto& frame = app.pads.frame();
    const auto& settings = app.padConfig.settings;
    const WorldPoint from{fromX, fromY};
    const StickVec left = frame.left;
    const float tilt = left.length();
    if (tilt > 0) {
        app.padAim = StickVec{left.x / tilt, left.y / tilt};
    }
    s.from = from;
    // (What liveSprites gathered last frame is used below, then gathered again.)
    const auto regather = [&] {
        s.enemies.clear();
        s.near.clear();
        s.collecting = true;
    };
    // The free cursor: the right stick moves it, the mouse's ways take it (the hover, the facing,
    // the cursor point at rest); the left stick still walks.
    if (s.freeCursor) {
        const bool had = s.hasPointer;
        s.dropAim();
        s.hasPointer = had;
        moveFreeCursor(app, frame.right, frame.seconds);
        s.enemies.clear();
        s.near.clear();
        s.collecting = false;
        if (tilt <= 0) {
            return PadSteer::None;
        }
        const WorldPoint p = ahead(from, *app.padAim, pad::stickReach(tilt, settings.runThreshold));
        client.move(facingTo(from, p));
        client.setCursor(static_cast<int>(p.x), static_cast<int>(p.y));
        client.setCursorSprite(std::nullopt);
        return PadSteer::Moves;
    }
    // The aim: the right stick's reticle, else the left stick's way, else the way we last went.
    const pad::RightStickFrame r = s.right.update(frame.right, frame.seconds, settings.sensitivity);
    s.reticle.reset();
    StickVec dir = app.padAim ? *app.padAim : ownFacingWay(app);
    if (r.reticle) {
        const float halfW = static_cast<float>(app.camera.viewWidth) / 2 / app.camera.zoom;
        const float halfH = static_cast<float>(app.camera.viewHeight) / 2 / app.camera.zoom;
        s.reticle = WorldPoint{from.x + r.reticle->x * halfW, from.y + r.reticle->y * halfH};
        if (pad::distance(from, *s.reticle) > 4) {
            dir = pad::wayTo(from, *s.reticle);
        }
    }
    s.dir = dir;
    pad::TargetFrame tf;
    tf.from = from;
    tf.dir = dir;
    tf.reticle = s.reticle;
    tf.idle = tilt <= 0 && !r.reticle && frame.right.length() <= 0;
    tf.enemies = s.enemies;
    tf.assist = settings.aimAssist;
    tf.lockMode = settings.lockMode;
    tf.lockHeld = lockHeld(app);
    tf.seconds = frame.seconds;
    tf.flick = r.flick;
    tf.step = std::exchange(s.pendingStep, 0);
    const auto before = s.target;
    s.target = s.targeter.update(tf);
    const pad::AimCandidate* target = enemyById(app, s.target);
    if (app.scriptPad && before != s.target) {
        // (--pad-script runs: the target's changes, for the test's log)
        SDL_Log("pad: target %d%s, %zu enemies in sight", s.target ? static_cast<int>(*s.target) : -1,
                s.targeter.locked() ? " (locked)" : "", s.enemies.size());
    }
    s.targetAt = target ? std::optional<WorldPoint>(target->at) : std::nullopt;
    // A cast's aim through its chant (following its creature).
    std::optional<WorldPoint> castAim;
    std::optional<std::uint16_t> castSprite;
    if (SDL_GetTicks() < s.castUntilMs && s.castAt) {
        if (const auto* c = enemyById(app, s.castTarget)) {
            castAim = c->at;
            castSprite = s.castTarget;
        } else {
            castAim = s.castAt;
        }
    } else {
        s.castAt.reset();
        s.castTarget.reset();
    }
    const WorldPoint aim = castAim ? *castAim : s.targetAt ? *s.targetAt : s.reticle ? *s.reticle : ahead(from, dir, pad::kWalkReach);
    const std::optional<std::uint16_t> sprite = castAim ? castSprite : s.targetAt ? s.target : std::nullopt;
    s.aimAt = aim;
    const auto hadInteract = interactKey(s.interact);
    pickInteract(app, from, dir);
    if (app.scriptPad && interactKey(s.interact) != hadInteract) {
        // (--pad-script runs: what A takes, for the test's log)
        SDL_Log("pad: A takes %lld (hand %d)", s.interact ? static_cast<long long>(*interactKey(s.interact)) : -1LL,
                s.interact ? s.interact->cursor : -1);
    }
    setPointer(app, aim);
    regather();
    if (tilt > 0) {
        // Moving: the cursor point out the stick's way by its tilt (walk or run), unless a chant's
        // aim holds it; with the attack held, a target close by and ahead is gone for.
        WorldPoint p = ahead(from, *app.padAim, pad::stickReach(tilt, settings.runThreshold));
        std::uint8_t way = facingTo(from, p);
        std::optional<std::uint16_t> pSprite;
        static const int kAction = InputBindings::control("Action");
        const bool attacking = app.input && app.input->held(kAction);
        if (castAim) {
            p = *castAim;
            pSprite = castSprite;
        } else if (attacking && s.targetAt && pad::distance(from, *s.targetAt) < pad::kRunDistance &&
                   pad::angleBetween(*app.padAim, pad::wayTo(from, *s.targetAt)) < 90) {
            p = *s.targetAt;
            pSprite = s.target;
            way = facingTo(from, p);
        }
        client.move(way);
        client.setCursor(static_cast<int>(p.x), static_cast<int>(p.y));
        client.setCursorSprite(pSprite);
        return PadSteer::Moves;
    }
    // At rest: facing the aim point (the target, the reticle, or the way we last went), the
    // cursor point on it.
    client.face(facingTo(from, aim));
    client.setCursor(static_cast<int>(aim.x), static_cast<int>(aim.y));
    client.setCursorSprite(sprite);
    return PadSteer::Holds;
}

// ---------------------------------------------------------------------------- buttons

bool padPlayPress(App& app, const pad::PadEdge& edge)
{
    auto& s = app.padPlay;
    if (!s.freeCursor || app.view3d.active()) {
        return false;
    }
    const std::uint8_t bit = edge.button == pad::PadButton::South ? 1 : edge.button == pad::PadButton::West ? 2 : 0;
    if (bit == 0) {
        return false;
    }
    if (!(s.freeButtons & bit)) {
        s.freeButtons |= bit;
        pushFreeMouse(app, SDL_EVENT_MOUSE_BUTTON_DOWN, bit == 1 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT);
    }
    return true;
}

bool padPlayRelease(App& app, const pad::PadEdge& edge)
{
    auto& s = app.padPlay;
    const std::uint8_t bit = edge.button == pad::PadButton::South ? 1 : edge.button == pad::PadButton::West ? 2 : 0;
    if (bit == 0 || !(s.freeButtons & bit)) {
        return false;
    }
    s.freeButtons &= static_cast<std::uint8_t>(~bit);
    pushFreeMouse(app, SDL_EVENT_MOUSE_BUTTON_UP, bit == 1 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT);
    return true;
}

bool padPlayControls(App& app, const pad::PadActionSet& actions, const std::vector<std::string>& controls)
{
    auto& s = app.padPlay;
    if (s.freeCursor || app.view3d.active() || !app.pads.padInUse()) {
        return false;
    }
    const bool slot = std::ranges::any_of(controls, isSlotControl);
    if (app.armed && (actions.has(pad::PadAction::Interact) || slot)) {
        fireArmed(app);
        return true;
    }
    for (const auto& c : controls) {
        if (isSlotControl(c)) {
            if (const auto aim = slotAim(app, static_cast<std::size_t>(c[10] - '1')); aim && aim->first != pad::SpellAim::Self) {
                aimCast(app, aim->first, aim->second);
            }
            break;
        }
        if (c == "PlaceTrapBomber" && !isWarrior(app)) {
            aimCast(app, pad::SpellAim::Point, 0);
            break;
        }
    }
    return false;
}

void padPlayAction(App& app, pad::PadAction action, bool down)
{
    auto& s = app.padPlay;
    if (!down || !app.live) {
        return;
    }
    switch (action) {
    case pad::PadAction::Interact:
        if (!s.freeCursor) {
            interact(app);
        }
        break;
    case pad::PadAction::TargetNext:
    case pad::PadAction::TargetPrevious:
        s.pendingStep = action == pad::PadAction::TargetNext ? 1 : -1;
        break;
    case pad::PadAction::FreeCursor:
        if (s.freeCursor) {
            releaseFreeButtons(app);
            s.freeCursor = false;
            s.hasPointer = false;
        } else {
            // From where the pad's pointer is (the aim point), else the view's middle.
            s.freeX = s.hasPointer ? s.pointerX : static_cast<float>(app.camera.viewWidth) / 2;
            s.freeY = s.hasPointer ? s.pointerY : static_cast<float>(app.camera.viewHeight) / 2;
            s.freeHeld = 0;
            s.freeCursor = true;
            s.dropAim();
            s.hasPointer = true;
            s.pointerX = s.freeX;
            s.pointerY = s.freeY;
            pushFreeMouse(app, SDL_EVENT_MOUSE_MOTION, 0);
        }
        uiSound(app, 0x399); // ShellClick
        break;
    default:
        break;
    }
}

void padPlayDropped(App& app)
{
    releaseFreeButtons(app);
}

// ----------------------------------------------------------------------- the drawables

bool padWants(const App& app, const nox::client::Drawable& d, const TypeArt& a)
{
    const auto& s = app.padPlay;
    const float dx = static_cast<float>(d.x) - s.from.x;
    const float dy = static_cast<float>(d.y) - s.from.y;
    const float reach = (a.classes & (oc::Monster | oc::Player)) ? kWantReach : kNearReach;
    return dx * dx + dy * dy <= reach * reach && s.enemies.size() < kMaxSeen && s.near.size() < kMaxSeen;
}

void padNoteSeen(App& app, std::uint16_t netCode, const nox::client::Drawable& d, const TypeArt& a)
{
    auto& s = app.padPlay;
    const WorldPoint at{static_cast<float>(d.x), static_cast<float>(d.y)};
    if (pad::distance(s.from, at) <= kNearReach) {
        s.near.push_back({netCode, at}); // (handCursor says what a hand makes of it)
    }
    if (!(a.classes & (oc::Monster | oc::Player))) {
        return;
    }
    const auto& client = app.live->client();
    const auto& world = client.world();
    pad::CreatureFacts facts;
    facts.player = (a.classes & oc::Player) != 0;
    if (facts.player) {
        const auto* p = world.player(netCode);
        if (!p || p->dead || (p->status & 1)) {
            return;
        }
        facts.team = p->team;
    } else {
        if (d.animState == 9 || d.animState == 10) {
            return; // dying or dead
        }
        facts.subclasses = a.subclasses;
        facts.talks = (d.xStatus & 0x10) != 0;
        facts.friendListed = world.isFriend(netCode);
        facts.partyCreature = app.partyDots && partyCreature(world, netCode, app.partyCreatures);
    }
    const auto* me = ownPlayer(client);
    if (pad::creatureSide(facts, me ? me->team : 0, world.game().gameFlags) == pad::Side::Enemy) {
        s.enemies.push_back({netCode, at, facts.player});
    }
}

// --------------------------------------------------------------------------- the draw

bool padHidesPointer(const App& app)
{
    return app.live && app.pads.padInUse() && app.padPlay.hasPointer && !app.padPlay.freeCursor && !app.view3d.active();
}

std::optional<View3D::Ring> padTargetRing(App& app)
{
    const auto& s = app.padPlay;
    if (!s.target || !app.live || !app.renderGui) {
        return std::nullopt;
    }
    const auto* d = app.live->client().world().drawable(*s.target);
    const TypeArt* art = d ? app.art->get(d->typeId) : nullptr;
    if (!art) {
        return std::nullopt;
    }
    const auto p = app.interpolator.position(*s.target, *d);
    // Locked: steady red; the soft target: the hover ring's flicker, dark red and red.
    const std::uint32_t pick = ((gameFrame(app) * 2654435761u) ^ (*s.target * 40503u)) >> 16;
    const std::uint8_t color = s.targeter.locked() ? Red : pick % 2 == 0 ? DarkRed : Red;
    return View3D::Ring{p.x, p.y, extentRing(art), paletteColor(color)};
}

std::optional<View3D::Ring> padInteractRing(App& app)
{
    const auto& s = app.padPlay;
    if (!padHidesPointer(app) || !s.interact || !app.renderGui) {
        return std::nullopt;
    }
    WorldPoint at = s.interact->at;
    if (s.interact->drawable) {
        if (const auto* d = app.live->client().world().drawable(*s.interact->drawable)) {
            const auto p = app.interpolator.position(*s.interact->drawable, *d);
            at = {p.x, p.y};
        }
    }
    const std::uint32_t pick = ((gameFrame(app) * 2654435761u) ^ (s.interact->extent * 40503u + 7u)) >> 16;
    const std::uint8_t color = pick % 3 == 0 ? White : pick % 3 == 1 ? LightBlue : Blue;
    return View3D::Ring{at.x, at.y, s.interact->radius, paletteColor(color)};
}

namespace {

/// What's printed on the button bound to `action` (its first binding), for a prompt.
std::string buttonLabel(const App& app, std::string_view action)
{
    for (const auto& b : app.padConfig.bindings) {
        if (b.action != action) {
            continue;
        }
        switch (app.pads.faceLabel(b.button)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return NOX_TR("game.pad.button.cross", "Cross");
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return NOX_TR("game.pad.button.circle", "Circle");
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return NOX_TR("game.pad.button.square", "Square");
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return NOX_TR("game.pad.button.triangle", "Triangle");
        default: return std::string(pad::padButtonName(b.button));
        }
    }
    return {};
}

} // namespace

void drawPadOverlay(App& app, float scale)
{
    drawPadPanels(app); // (the panels' snap cursor, pad_panels.cpp)
    const auto& s = app.padPlay;
    // The reticle while the right stick aims (and a moment after).
    if (s.reticle && app.cursor) {
        const SDL_FPoint at = toScreen(app, *s.reticle);
        app.cursor->draw(app.renderer, CursorType::Target, at.x, at.y, std::max(scale, 1.0f));
    }
    // A's prompt over what it takes.
    if (s.interact && app.font && app.renderGui) {
        std::string_view word;
        switch (static_cast<CursorType>(s.interact->cursor)) {
        case CursorType::Pickup:
        case CursorType::Caution: word = NOX_TR("game.pad.prompt.pickup", "Pick up"); break;
        case CursorType::Talk: word = NOX_TR("game.pad.prompt.talk", "Talk"); break;
        case CursorType::Trade: word = NOX_TR("game.pad.prompt.trade", "Trade"); break;
        case CursorType::Use: word = NOX_TR("game.pad.prompt.use", "Use"); break;
        default: return;
        }
        const std::string button = buttonLabel(app, "Interact");
        const std::string text = button.empty() ? std::string(word) : "[" + button + "] " + std::string(word);
        WorldPoint at = s.interact->at;
        if (s.interact->drawable) {
            if (const auto* d = app.live->client().world().drawable(*s.interact->drawable)) {
                const auto p = app.interpolator.position(*s.interact->drawable, *d);
                at = {p.x, p.y};
            }
        }
        const SDL_FPoint sp = toScreen(app, at);
        const int size = std::clamp(static_cast<int>(std::lround(11 * app.camera.zoom)), 9, 64);
        const float w = app.font->measure(text, size);
        const float x = std::round(sp.x - w / 2);
        const float y = std::round(sp.y - s.interact->radius * app.camera.zoom - 2.2f * static_cast<float>(size));
        const float shadow = std::max(1.0f, std::round(app.camera.zoom));
        app.font->draw(text, x + shadow, y + shadow, size, {8, 8, 8, 255});
        app.font->draw(text, x, y, size, {255, 255, 255, 255});
    }
}

} // namespace noxsdl
