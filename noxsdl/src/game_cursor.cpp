// The game's cursor: which one this frame shows (chooseCursor, the game's cursor update) and its
// draw at the mouse (drawCursor). What the world's pick gives is cursor_pick.hpp's; the images are
// cursor.hpp's.

#include "app.hpp"
#include "book_award.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/things/object_class.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace noxsdl {

/// cursorUpdate's gate on its world part (pick::worldSkipped): a cinematic's widescreen, the game
/// we host paused (a solo game's pause: the game's flag the client reads), our player dying or
/// dead or without a drawable (playerAnimCheck: animation state 1, 2 or 0x33).
pick::Situation worldGate(App& app)
{
    pick::Situation s;
    s.wideScreen = app.cinematics && app.cinematics->wideScreen();
    s.paused = app.host && (app.host->gameFlags() & nox::server::GameFlag::Paused) != 0;
    s.dying = bookaward::dyingOrDead(ownAnimState(app));
    return s;
}

/// Live: this frame's cursor (cursorUpdate): the pointer over the in-game
/// menu; else the arrow, unless we're playing and the cursor's pick is an item (the pickup hand
/// within 75, the far one beyond), a creature to talk to or a shopkeeper within 125, or a useable
/// thing within 75.
void chooseCursor(App& app, bool inGame)
{
    namespace oc = nox::things::ObjectClass;
    // The Escape menu, and the windows it opens over the game (Options, the key bindings): the
    // still pointer anywhere (cursorUpdate: a modal window up sets cursor 0).
    if (menuWindowUp(app)) {
        app.cursorType = CursorType::Select;
        return;
    }
    if (app.armed) {
        app.cursorType = CursorType::Target; // a spell waiting for its target (video_cursorDrawImpl)
        return;
    }
    // cursorUpdate's start: the move cursor, but the arrow under a cinematic's widescreen, where
    // the world's cursors (Talk, Trade, the pickup hands, Use) are skipped: a click then starts no
    // talk (Cli_ProcessControlEvents acts on the cursor).
    const bool wide = app.cinematics && app.cinematics->wideScreen();
    app.cursorType = wide ? CursorType::Select : CursorType::Move;
    const auto& client = app.live->client();
    if (app.inventory && inGame) {
        // Identify mode's hand over the tray and the magnifier, its no-hand elsewhere.
        const SDL_FPoint m = mousePos(app);
        if (const auto c = app.inventory->identifyCursor(m.x, m.y, app.camera.viewWidth, app.camera.viewHeight)) {
            app.cursorType = static_cast<CursorType>(*c);
            return;
        }
    }
    if (app.tradeScreens && inGame) {
        // Over the shop, dialog or trade window the arrow, or the shop mode's Buy / Sell /
        // Repair hand; over the inventory in Sell or Repair mode, that hand.
        const SDL_FPoint m = mousePos(app);
        const int w = app.camera.viewWidth;
        const int h = app.camera.viewHeight;
        const bool overShop = app.tradeScreens->contains(m.x, m.y, w, h);
        const bool overInventory = app.inventory && app.inventory->contains(m.x, m.y, w, h);
        if (overShop || (overInventory && app.tradeScreens->shopOpen())) {
            const auto mode = app.tradeScreens->shopCursor();
            app.cursorType = mode && (overShop ? *mode == 11 : *mode != 11) ? static_cast<CursorType>(*mode)
                                                                            : CursorType::Select;
            return;
        }
    }
    if (app.book && inGame) {
        const SDL_FPoint m = mousePos(app);
        if (app.book->contains(m.x, m.y, app.camera.viewWidth, app.camera.viewHeight)) {
            app.cursorType = CursorType::Select;
            return;
        }
    }
    if (app.hud && inGame) {
        const SDL_FPoint m = mousePos(app);
        const bool pieces = gameHudShown(app); // (hidden: not there to point at; an open inventory still is)
        if ((pieces && app.hud->hit(client.world(), client.netCode(), quickbarState(app), m.x, m.y, app.camera.viewWidth,
                                    app.camera.viewHeight)) ||
            (app.inventory && (pieces || app.inventory->isOpen()) && app.inventory->contains(m.x, m.y, app.camera.viewWidth, app.camera.viewHeight)) ||
            (pieces && app.summon && app.summon->contains(m.x, m.y, app.camera.viewWidth, app.camera.viewHeight))) {
            app.cursorType = CursorType::Select; // over the GUI
            return;
        }
    }
    if (inGame && !wide) {
        // A mod's clickable label (a click there is the mode's): the pickup hand.
        const SDL_FPoint m = mousePos(app);
        for (const auto& [id, ui] : app.modeUis) {
            if (ui->clickableAt(app, m.x, m.y)) {
                app.cursorType = CursorType::Pickup;
                return;
            }
        }
    }
    const auto* me = ownPlayer(client);
    if (!inGame || !playing(client) || (!app.cursorHovered && !app.cursorUseable)) {
        return;
    }
    const auto self = app.anchors.find(client.netCode());
    if (self == app.anchors.end()) {
        return;
    }
    // cursorUpdate, on the cursor's pick (cursor_pick.hpp's worldCursor): how far it is from us
    // in whole units, what it is, whether we're in a dialog or a shop or dragging an item.
    const auto dist2 = [&](float x, float y) {
        const std::int64_t dx = std::lround(x) - std::lround(self->second.x);
        const std::int64_t dy = std::lround(y) - std::lround(self->second.y);
        return dx * dx + dy * dy;
    };
    // (Its world part skipped under the widescreen, paused, or dying: worldGate.)
    pick::Situation situation = worldGate(app);
    situation.busy = client.world().dialog().open || (app.tradeScreens && app.tradeScreens->shopOpen());
    situation.dragging = app.inventory && app.inventory->dragging();
    pick::Target target;
    if (app.cursorUseable) {
        // A useable map object (enabled: useableAt takes no other).
        const TypeArt* a = app.cursorUseable->art;
        if (!a) {
            return;
        }
        target.classes = a->classes;
        target.subclasses = a->subclasses;
        target.dist2 = dist2(app.cursorUseable->x, app.cursorUseable->y);
    } else {
        const auto* d = client.world().drawable(*app.cursorHovered);
        const TypeArt* a = d ? app.art->get(d->typeId) : nullptr;
        const auto there = app.anchors.find(*app.cursorHovered);
        if (!a || there == app.anchors.end()) {
            return;
        }
        target.classes = a->classes;
        target.subclasses = a->subclasses;
        target.xStatus = d->xStatus;
        target.enabled = d->enabled;
        target.mimicAsleep = a == app.art->get(app.mimicArt, "Mimic") && d->animState == 0;
        target.dist2 = dist2(there->second.x, there->second.y);
        if (a->classes & oc::Pickup) {
            // An item our class can't use gets Caution (not in solo or quest games): weapons and
            // armour by modifier.bin's CLASSUSE (none without a record or without the line: the
            // game's record byte zeroed), mana potions for a warrior (FOOD SUBCLASS bit 5), a glyph
            // for anyone but a wizard. (noxcpp: none but the glyph's when the host's mode lets any
            // class pick anything up, ServerFeature::PickupAnyClass in its extended reply.)
            const int cls = me ? me->playerClass : 0;
            std::uint8_t record = 0;
            if (a->classes & (oc::Weapon | oc::Wand)) {
                const auto* def = app.modifiers.findWeapon(a->name);
                record = def ? def->classUse.value_or(0) : 0;
            } else if (a->classes & oc::Armor) {
                const auto* def = app.modifiers.findArmor(a->name);
                record = def ? def->classUse.value_or(0) : 0;
            }
            const bool soloOrQuest = (client.world().game().gameFlags & 0x1800) != 0;  // 0x800 solo, 0x1000 quest
            const auto& host = app.live->extended();
            const bool anyClass = host && (host->features & nox::lobby::ServerFeature::PickupAnyClass);
            situation.canUse = pick::classMayTake(pick::itemClassUse(a->classes, a->subclasses, record), cls, a->name == "Glyph", soloOrQuest, anyClass);
        }
    }
    switch (pick::worldCursor(target, situation)) {
    case pick::WorldCursor::Pickup: app.cursorType = CursorType::Pickup; break;
    case pick::WorldCursor::PickupFar: app.cursorType = CursorType::PickupFar; break;
    case pick::WorldCursor::Caution: app.cursorType = CursorType::Caution; break;
    case pick::WorldCursor::Talk: app.cursorType = CursorType::Talk; break;
    case pick::WorldCursor::Trade: app.cursorType = CursorType::Trade; break;
    case pick::WorldCursor::Use: app.cursorType = CursorType::Use; break;
    case pick::WorldCursor::None: break;
    }
}

/// Draws the game's cursor at the mouse (not when the mouse is outside the window).
void drawCursor(App& app, CursorType type, float scale)
{
    if (!app.cursor || (!app.testMouse && SDL_GetMouseFocus() != app.window)) {
        return;
    }
    if (gameInPanel(app) && !app.playTestCaptured) {
        return; // (a play-test in the editor's Map panel, the mouse the editor's: its pointer the system's)
    }
    if (app.view3d.looking()) {
        // The 3D view's mouse look: the cursor's kind as its crosshair's colour.
        auto reticle = View3D::Reticle::Plain;
        if (type == CursorType::Pickup || type == CursorType::PickupFar) {
            reticle = View3D::Reticle::Pickup;
        } else if (type == CursorType::Caution) {
            reticle = View3D::Reticle::Caution;
        } else if (type == CursorType::Use) {
            reticle = View3D::Reticle::Use;
        } else if (type == CursorType::Talk || type == CursorType::Trade) {
            reticle = View3D::Reticle::Talk;
        } else if (type == CursorType::Target) {
            reticle = View3D::Reticle::Target;
        } else if (app.live && app.hovered) {
            reticle = hoveredEnemy(app) ? View3D::Reticle::Enemy : View3D::Reticle::Friend;
        }
        app.view3d.drawCrosshair(app.renderer, reticle);
        return;
    }
    const SDL_FPoint m = mousePos(app);
    int frame = 0;
    std::optional<SDL_Color> tint;
    if (type == CursorType::Move) {
        bool running = false; // our player in RUN
        if (app.live) {
            const auto* self = app.live->client().world().drawable(app.live->client().netCode());
            running = self && self->animState == 6;
        }
        frame = Cursor::moveFrame(m.x / scale, m.y / scale, static_cast<float>(app.camera.viewWidth) / scale,
                                  static_cast<float>(app.camera.viewHeight) / scale, running);
        // Over something, the arrow's colours are multiplied by red for an enemy, else by blue
        // (video_cursorDrawImpl case 14).
        if (app.live && app.hovered) {
            tint = hoveredEnemy(app) ? SDL_Color{255, 0, 0, 255} : SDL_Color{0, 160, 255, 255};
        }
    }
    app.cursor->draw(app.renderer, type, m.x, m.y, std::max(scale, 1.0f), frame, tint);
}

} // namespace noxsdl
