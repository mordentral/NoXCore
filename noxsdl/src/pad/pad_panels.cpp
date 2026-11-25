// (noxcpp) The game controller's snap cursor on the game's panels (pad_panels.hpp).
#include "pad/pad_panels.hpp"

#include "gui_focus_frame.hpp"
#include "pad/pad_glyphs.hpp"
#include "pad/pad_play.hpp"
#include "pointer_map.hpp"

#include "noxcore/strings/l10n.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

namespace {

using pad::NavBox;
using pad::NavDir;
using pad::PadButton;
using pad::Panel;
using pad::PanelPress;
using pad::Spot;
using Holding = pad::PanelState::Holding;

/// Each panel's spots, gathered into (kept, so a frame allocates nothing once they've grown).
std::vector<Inventory::PadSpot> invSpots;
std::vector<Spellbook::PadSpot> bookSpots;
std::vector<SDL_FRect> rects;
std::vector<ModeUi::PadSpot> modeSpots;
std::vector<NavBox> boxes;
std::vector<int> groups;

NavBox boxOf(const SDL_FRect& r)
{
    return {r.x, r.y, r.w, r.h};
}

SDL_FRect rectOf(const NavBox& b)
{
    return {b.x, b.y, b.w, b.h};
}

SDL_FPoint middle(const Spot& s)
{
    return {s.box.cx(), s.box.cy()};
}

Inventory::PadSpot invSpot(const Spot& s)
{
    return {static_cast<Inventory::PadSpotKind>(s.kind), s.index, rectOf(s.box)};
}

Spellbook::PadSpot bookSpot(const Spot& s)
{
    return {static_cast<Spellbook::PadSpotKind>(s.kind), s.index, rectOf(s.box)};
}

void log(const App& app, const char* what, const Spot& s)
{
    if (app.scriptPad) {
        // (--pad-script runs: the cursor's steps and actions, for the test's log)
        SDL_Log("pad panels: %s panel %d kind %d index %d at %.0f, %.0f", what, static_cast<int>(s.panel), static_cast<int>(s.kind), s.index,
                s.box.cx(), s.box.cy());
    }
}

/// Whether the snap cursor may have the pad now: in a game the pad drives (no menu, chat, console,
/// briefing's screen, 3D view, spell waiting for its target, amount box), and no other window
/// system screen has the pad.
bool allowed(const App& app)
{
    if (!app.live || !app.pads.padInUse() || !padGameInput(app) || padHasMenus(app) || app.view3d.active() || app.armed ||
        (app.cinematics && app.cinematics->screenUp())) {
        return false;
    }
    if (app.tradeScreens && app.tradeScreens->modalUp()) {
        return false;
    }
    const gui::Gui* owner = gui::navOwner();
    return !owner || (app.tradeScreens && app.tradeScreens->hasPad());
}

bool tradeWindowsUp(const App& app)
{
    return app.tradeScreens && (app.tradeScreens->shopOpen() || app.tradeScreens->tradeOpen() || app.tradeScreens->dialogOpen());
}

/// This frame's spots into app.padPanels.spots; whether one of them has the stick (the inventory,
/// the book: the player stands).
bool gather(App& app)
{
    auto& out = app.padPanels.spots;
    out.clear();
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const float scale = Hud::scale(w, h);
    const auto& world = app.live->client().world();
    bool still = false;
    if (app.inventory && app.inventory->isOpen()) {
        app.inventory->padSpots(w, h, world, app.padPanels.holding == Holding::Item, invSpots);
        for (const auto& s : invSpots) {
            out.push_back({Panel::Inventory, static_cast<std::uint8_t>(s.kind), s.index, boxOf(s.box)});
        }
        still = still || !invSpots.empty();
    }
    if (app.book && app.book->isOpen()) {
        app.book->padSpots(w, h, bookSpots);
        for (const auto& s : bookSpots) {
            out.push_back({Panel::Book, static_cast<std::uint8_t>(s.kind), s.index, boxOf(s.box)});
        }
        still = still || !bookSpots.empty();
    }
    const bool pieces = gameHudShown(app) && !(app.tradeScreens && app.tradeScreens->hidesHud()); // (a shop's, a dialog's: none)
    if (still && pieces && app.hud) {
        // The bars beside them: the set in use (all five while they're all showing), the trap tray.
        for (int set = 0; set < 5; ++set) {
            if (!app.barsExpanded && set != app.bars.row) {
                continue;
            }
            for (int slot = 0; slot < 5; ++slot) {
                const SDL_FPoint p = Hud::slotPosition(w, h, set, slot, app.barsExpanded, app.bars.row);
                out.push_back({Panel::Bars, 0, set * 5 + slot, NavBox{p.x * scale, p.y * scale, 34 * scale, 34 * scale}});
            }
        }
        if (app.trapOpen && !isWarrior(app)) {
            for (int slot = 0; slot < 3; ++slot) {
                const SDL_FPoint p = Hud::trapSlotPosition(w, h, slot);
                out.push_back({Panel::Traps, 0, slot, NavBox{p.x * scale, p.y * scale, 34 * scale, 34 * scale}});
            }
        }
    }
    if (app.summon) {
        // The cage with the panels (it's a HUD piece); its command list whenever it's up (it takes
        // every click then: its lines alone).
        const bool list = app.summon->padSpots(world, w, h, rects);
        if (list) {
            out.clear();
        }
        if (list || (still && pieces)) {
            for (std::size_t i = 0; i < rects.size(); ++i) {
                out.push_back({Panel::Summon, static_cast<std::uint8_t>(list ? 1 : 0), static_cast<int>(i), boxOf(rects[i])});
            }
        }
        if (list) {
            return still;
        }
    }
    if (app.mapVote && app.mapVote->shown()) {
        app.mapVote->padCards(app.live->client().mapVote(), w, h, scale, rects);
        for (std::size_t i = 0; i < rects.size(); ++i) {
            out.push_back({Panel::MapVote, 0, static_cast<int>(i), boxOf(rects[i])});
        }
    }
    modeSpots.clear();
    for (const auto& [id, ui] : app.modeUis) {
        ui->padSpots(app, w, h, scale, modeSpots);
    }
    for (std::size_t i = 0; i < modeSpots.size(); ++i) {
        out.push_back({Panel::Mode, 0, static_cast<int>(i), boxOf(modeSpots[i].box), modeSpots[i].escapes});
    }
    return still;
}

void fillBoxes(const App& app)
{
    boxes.clear();
    for (const auto& s : app.padPanels.spots) {
        boxes.push_back(s.box);
    }
}

/// The selection kept on its spot (its box this frame), or, gone, on the nearest of its panel's
/// (else of all); a first one on the inventory's first cell, the book's first entry, the first.
void settle(App& app)
{
    auto& s = app.padPanels;
    if (s.spots.empty()) {
        s.at.reset();
        return;
    }
    if (s.at) {
        for (const auto& sp : s.spots) {
            if (sp.sameAs(*s.at)) {
                s.at = sp;
                return;
            }
        }
        // Gone (scrolled out, a page turned, its window shut): the nearest of its panel's, else any.
        std::optional<std::size_t> best;
        float bestD = 0;
        for (std::size_t i = 0; i < s.spots.size(); ++i) {
            if (s.spots[i].panel != s.at->panel) {
                continue;
            }
            const float dx = s.spots[i].box.cx() - s.at->box.cx();
            const float dy = s.spots[i].box.cy() - s.at->box.cy();
            const float d = dx * dx + dy * dy;
            if (!best || d < bestD) {
                best = i;
                bestD = d;
            }
        }
        if (best) {
            s.at = s.spots[*best];
            return;
        }
        // (its panel shut: the one up now starts as it would)
    }
    for (const auto& sp : s.spots) {
        if (sp.panel == Panel::Inventory && sp.kind == static_cast<std::uint8_t>(Inventory::PadSpotKind::Cell)) {
            s.at = sp;
            return;
        }
    }
    for (const auto& sp : s.spots) {
        if (sp.panel == Panel::Book && sp.kind == static_cast<std::uint8_t>(Spellbook::PadSpotKind::Entry)) {
            s.at = sp;
            return;
        }
    }
    s.at = s.spots.front();
}

void putPointer(App& app, SDL_FPoint p)
{
    app.padPlay.hasPointer = true;
    app.padPlay.pointerX = p.x;
    app.padPlay.pointerY = p.y;
}

/// A left click at output point (x, y) through the game's own way for the left button
/// (leftMouse: the cage, the book, the inventory, the bars).
void clickHere(App& app, SDL_FPoint p)
{
    leftMouse(app, true, p.x, p.y);
    leftMouse(app, false, p.x, p.y);
}

/// A left click at output point `p` sent through the events, as the mouse's (the windows over the
/// game take it in their order: the map vote, the modes'), the pad's mouse id (it never hands the
/// device back to the mouse).
void pushClick(App& app, SDL_FPoint p)
{
    float x = p.x;
    float y = p.y;
    pointermap::toWindow(x, y, app.pixelDensity, pointermap::Area{app.gameArea.x, app.gameArea.y, app.gameArea.w, app.gameArea.h},
                         app.letterboxed);
    for (const bool down : {true, false}) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.which = pad::GamepadManager::kPadMouse;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = down;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
        SDL_PushEvent(&e);
    }
}

void pushEscape()
{
    for (const bool down : {true, false}) {
        SDL_Event e = keyEvent(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, down);
        SDL_PushEvent(&e);
    }
}

/// B on the panels: as Escape (a spell's target put down, the shop left, the book and the
/// inventory closed).
void back(App& app)
{
    liveControls(app, {"ToggleQuitMenu"});
}

Inventory::Actions inventoryActions(App& app)
{
    auto& client = app.live->client();
    const bool say = app.scriptPad != nullptr; // (--pad-script runs: what's asked, for the test's log)
    const auto told = [say](const char* what, std::uint16_t nc) {
        if (say) {
            SDL_Log("pad panels: inventory asks %s %u", what, static_cast<unsigned>(nc));
        }
    };
    Inventory::Actions a;
    a.use = [&client, told](std::uint16_t nc) { told("use", nc); client.use(nc); };
    a.equip = [&client, told](std::uint16_t nc) { told("equip", nc); client.equip(nc); };
    a.unequip = [&client, told](std::uint16_t nc) { told("unequip", nc); client.unequip(nc); };
    a.drop = [&client, told](std::uint16_t nc, int wx, int wy) { told("drop", nc); client.dropAt(nc, wx, wy); };
    a.secondary = [&client, told](std::uint16_t nc) { told("secondary", nc); client.setSecondaryWeapon(nc); };
    a.putAwayWeapon = [&client, told](std::uint16_t nc) { told("putAwayWeapon", nc); client.putAwayWeapon(nc); };
    a.weaponBit = [&client](std::uint16_t typeId) { return client.weaponBit(typeId); };
    return a;
}

/// Where the pad drops things: at our feet.
SDL_Point ground(const App& app)
{
    const auto at = app.live->client().position();
    return at ? SDL_Point{at->first, at->second} : SDL_Point{};
}

/// The trade screens' windows have the pad's directions (their own navigation), or the cursor.
void handToWindows(App& app, bool to)
{
    app.padPanels.inWindows = to;
    if (app.tradeScreens) {
        app.tradeScreens->setPadNavigation(to);
    }
    if (app.scriptPad) {
        SDL_Log("pad panels: the pad %s the shop's windows", to ? "to" : "back from");
    }
}

/// A step `dir` from the selected spot.
void step(App& app, NavDir dir)
{
    auto& s = app.padPanels;
    if (!s.at) {
        return;
    }
    const Spot cur = *s.at;
    if (cur.panel == Panel::Inventory && app.inventory) {
        const auto kind = static_cast<Inventory::PadSpotKind>(cur.kind);
        if (kind == Inventory::PadSpotKind::Cell) {
            // The tray scrolled at the edge of the rows in view.
            const int col = cur.index / InventoryGrid::kRows;
            const int row = cur.index % InventoryGrid::kRows;
            const pad::TrayView view{app.inventory->trayFirstRow(), 3,
                                     pad::trayLastRow(app.inventory->trayLastFilledRow(), s.holding == Holding::Item)};
            if (const auto t = pad::trayEdgeStep(row, view, dir)) {
                app.inventory->padScroll(t->scroll);
                s.at->index = InventoryGrid::index(col, t->row);
                gather(app);
                // (that cell empty, so no stop: the filled one of the row nearest its column; the
                // tray fills row by row, so the row's last filled one at most)
                const auto cellAt = [&](int c) {
                    return std::ranges::find_if(s.spots, [&](const Spot& sp) {
                        return sp.panel == Panel::Inventory && sp.kind == static_cast<std::uint8_t>(Inventory::PadSpotKind::Cell) &&
                               sp.index == InventoryGrid::index(c, t->row);
                    });
                };
                for (int c = col; c >= 0; --c) {
                    if (const auto it = cellAt(c); it != s.spots.end()) {
                        s.at = *it;
                        break;
                    }
                }
                settle(app);
                log(app, "scrolled to", *s.at);
                return;
            }
        } else if (kind == Inventory::PadSpotKind::Journal && (dir == NavDir::Up || dir == NavDir::Down)) {
            app.inventory->padScroll(dir == NavDir::Up ? -1 : 1);
            return;
        }
    }
    fillBoxes(app);
    const auto next = pad::navPick(boxes, cur.box, dir);
    if (!next) {
        // Off the book's side: the page turned (its arrow's click).
        if (cur.panel == Panel::Book && (dir == NavDir::Left || dir == NavDir::Right)) {
            const auto arrow = dir == NavDir::Left ? Spellbook::PadSpotKind::Back : Spellbook::PadSpotKind::Forward;
            for (const auto& sp : s.spots) {
                if (sp.panel == Panel::Book && sp.kind == static_cast<std::uint8_t>(arrow)) {
                    clickHere(app, middle(sp));
                    break;
                }
            }
            return;
        }
        // Off the spots towards the shop's (the dialog's, the trade's) window: theirs now.
        if (tradeWindowsUp(app)) {
            if (const auto r = app.tradeScreens->padBounds(app.camera.viewWidth, app.camera.viewHeight)) {
                const std::array<NavBox, 1> window{boxOf(*r)};
                if (pad::navPick(window, cur.box, dir)) {
                    handToWindows(app, true);
                }
            }
        }
        return;
    }
    s.at = s.spots[*next];
    log(app, "to", *s.at);
}

/// The right stick's jump `dir`: out of the selected spot's group (the tray, a column, a bar...)
/// to the nearest spot of another that way; none on the panels that way, the shop's (the dialog's,
/// the trade's) windows when they're that way. Nothing else that way: nothing.
void jump(App& app, NavDir dir)
{
    auto& s = app.padPanels;
    if (!s.at) {
        return;
    }
    fillBoxes(app);
    groups.clear();
    for (const auto& sp : s.spots) {
        groups.push_back(pad::spotGroup(sp));
    }
    if (const auto next = pad::navJump(boxes, groups, s.at->box, pad::spotGroup(*s.at), dir)) {
        s.at = s.spots[*next];
        log(app, "jumped to", *s.at);
        return;
    }
    if (tradeWindowsUp(app)) {
        if (const auto r = app.tradeScreens->padBounds(app.camera.viewWidth, app.camera.viewHeight)) {
            const std::array<NavBox, 1> window{boxOf(*r)};
            if (pad::navPick(window, s.at->box, dir)) {
                handToWindows(app, true);
            }
        }
    }
}

/// A press on the inventory's spot (`p` its action).
void inventoryPress(App& app, const Spot& cur, PanelPress p)
{
    auto& s = app.padPanels;
    Inventory& inv = *app.inventory;
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const auto& world = app.live->client().world();
    const Inventory::PadSpot spot = invSpot(cur);
    if (s.holding == Holding::Bar) {
        return; // (a bar's spell goes on the bars)
    }
    if (s.holding == Holding::Item) {
        if (p == PanelPress::Accept || p == PanelPress::Move) {
            inv.padPlace(spot, world, inventoryActions(app));
            s.holding = Holding::None;
            log(app, "put down at", cur);
        } else if (p == PanelPress::Back) {
            inv.cancelDrag();
            s.holding = Holding::None;
        }
        return;
    }
    switch (p) {
    case PanelPress::Accept:
        if (spot.kind == Inventory::PadSpotKind::Secondary) {
            // The secondary box: weapons swapped (the right button's there).
            if (app.live->client().secondaryWeapon() != 0 && app.live->client().swapWeapons()) {
                uiSound(app, 0x37F); // NextWeapon
            }
            return;
        }
        inv.padAccept(spot, w, h, ground(app), world, inventoryActions(app));
        log(app, "accept", cur);
        return;
    case PanelPress::Move:
        if (inv.padTake(spot, w, h, world, static_cast<std::uint32_t>(SDL_GetTicks()))) {
            s.holding = Holding::Item;
            log(app, "picked up", cur);
        }
        return;
    case PanelPress::Drop:
        if (inv.padItemAction(spot, DragEnd::Outside, w, h, ground(app), world, inventoryActions(app))) {
            log(app, "dropped", cur);
        }
        return;
    case PanelPress::Secondary:
        if (inv.padItemAction(spot, DragEnd::SecondaryBox, w, h, ground(app), world, inventoryActions(app))) {
            log(app, "made secondary", cur);
        }
        return;
    case PanelPress::Back:
        back(app);
        return;
    default:
        return;
    }
}

/// A press on the book's spot.
void bookPress(App& app, const Spot& cur, PanelPress p, PadButton b)
{
    const auto kind = static_cast<Spellbook::PadSpotKind>(cur.kind);
    if (app.padPanels.holding != Holding::None) {
        return;
    }
    if (kind == Spellbook::PadSpotKind::Entry || kind == Spellbook::PadSpotKind::Icon) {
        // A slot button: the entry onto that slot of the set in use.
        if (const auto slot = pad::slotOfButton(app.padConfig, b)) {
            if (const std::uint32_t id = app.book->padSpell(bookSpot(cur), bookContext(app))) {
                bookSpellToSlot(app, id, *slot);
                if (app.scriptPad) {
                    SDL_Log("pad panels: book entry %d (spell %u) to slot %d of set %d", cur.index, id, *slot + 1, app.bars.row + 1);
                }
            }
            return;
        }
    }
    switch (p) {
    case PanelPress::Accept:
        clickHere(app, middle(cur));
        log(app, "clicked", cur);
        return;
    case PanelPress::PreviousPage:
    case PanelPress::Secondary: {
        const auto arrow = p == PanelPress::PreviousPage ? Spellbook::PadSpotKind::Back : Spellbook::PadSpotKind::Forward;
        for (const auto& sp : app.padPanels.spots) {
            if (sp.panel == Panel::Book && sp.kind == static_cast<std::uint8_t>(arrow)) {
                clickHere(app, middle(sp));
                return;
            }
        }
        return;
    }
    case PanelPress::Back:
        back(app);
        return;
    default:
        return;
    }
}

bool onBarSlot(const Spot& s)
{
    return s.panel == Panel::Bars || s.panel == Panel::Traps;
}

/// The bar slot's or the trap tray slot's spell (its id; 0 empty).
std::uint32_t slotSpell(const App& app, const Spot& s)
{
    if (s.panel == Panel::Bars) {
        return app.bars.rows[static_cast<std::size_t>(s.index / 5)][static_cast<std::size_t>(s.index % 5)].id;
    }
    return s.panel == Panel::Traps ? app.bars.currentTrap()[static_cast<std::size_t>(s.index)].id : 0;
}

/// A press with the bars' spells (pad::barAct): on a bar slot or a trap tray slot, or anywhere while
/// a bar's spell is held. Whether it was theirs (else the spot's panel's).
bool barPress(App& app, const Spot& cur, PanelPress p)
{
    auto& s = app.padPanels;
    if (s.holding == Holding::Item) {
        return onBarSlot(cur); // (an item goes in the inventory: on a slot, nothing)
    }
    switch (pad::barAct(p, onBarSlot(cur), s.holding == Holding::Bar)) {
    case pad::BarAct::Panel:
        return false;
    case pad::BarAct::None:
        return true;
    case pad::BarAct::PickUp:
        // The mouse's way: the press picks the spell up, the release on another slot swaps.
        leftMouse(app, true, cur.box.cx(), cur.box.cy());
        if (app.barDrag) {
            s.holding = Holding::Bar;
            log(app, "spell picked up at", cur);
        }
        return true;
    case pad::BarAct::PutDown:
        leftMouse(app, false, cur.box.cx(), cur.box.cy());
        s.holding = Holding::None;
        log(app, "spell put down at", cur);
        return true;
    case pad::BarAct::DropOff:
        // Let go off the bar, as the mouse's drag there: its slot emptied.
        dropBarDragOff(app);
        s.holding = Holding::None;
        log(app, "spell dropped off the bar at", cur);
        return true;
    case pad::BarAct::Cancel:
        app.barDrag.reset();
        s.holding = Holding::None;
        log(app, "spell put back from", cur);
        return true;
    case pad::BarAct::Empty:
        // Emptied, as a drag off the bar empties it.
        if (cur.panel == Panel::Bars) {
            app.bars.rows[static_cast<std::size_t>(cur.index / 5)][static_cast<std::size_t>(cur.index % 5)] = {};
        } else {
            app.bars.currentTrap()[static_cast<std::size_t>(cur.index)] = {};
        }
        log(app, "emptied", cur);
        return true;
    case pad::BarAct::Leave:
        back(app);
        return true;
    }
    return true;
}

} // namespace

PanelFrame padPanelsFrame(App& app)
{
    auto& s = app.padPanels;
    if (!allowed(app)) {
        padPanelsIdle(app);
        return PanelFrame::None;
    }
    // A move under way that its panel let go (closed, dropped by the mouse) is over.
    if ((s.holding == Holding::Item && !(app.inventory && app.inventory->dragging())) || (s.holding == Holding::Bar && !app.barDrag)) {
        s.holding = Holding::None;
    }
    const bool still = gather(app);
    if (s.spots.empty()) {
        padPanelsIdle(app);
        return PanelFrame::None;
    }
    s.active = true;
    s.still = still;
    if (s.inWindows && !tradeWindowsUp(app)) {
        handToWindows(app, false);
    }
    if (app.tradeScreens) {
        app.tradeScreens->setPadNavigation(s.inWindows);
    }
    settle(app);
    // A held direction: the D-pad's, and the left stick's on the panels that have it.
    if (!s.inWindows) {
        std::optional<NavDir> held;
        for (const PadButton b : {PadButton::DpadUp, PadButton::DpadDown, PadButton::DpadLeft, PadButton::DpadRight}) {
            if (app.pads.isDown(b)) {
                held = pad::dpadDir(b);
                break;
            }
        }
        if (still) {
            s.stick = pad::stickDir(app.pads.frame().left, s.stick);
            if (!held) {
                held = s.stick;
            }
        } else {
            s.stick.reset();
        }
        if (const auto d = s.repeat.frame(held, SDL_GetTicks())) {
            step(app, *d);
        }
        // The right stick on the inventory's and the book's screens: out of the group that way.
        if (still) {
            s.right = pad::stickDir(app.pads.frame().right, s.right);
            if (const auto d = s.jumpRepeat.frame(s.right, SDL_GetTicks())) {
                jump(app, *d);
            }
        } else {
            s.right.reset();
            s.jumpRepeat.clear();
        }
    }
    if (still) {
        padPlayIdle(app); // (no aim, no target: the player stands)
        padPanelsPointer(app);
        return PanelFrame::Still;
    }
    return PanelFrame::Dpad;
}

void padPanelsPointer(App& app)
{
    auto& s = app.padPanels;
    if (!s.active) {
        return;
    }
    if (s.inWindows && app.tradeScreens) {
        if (const auto p = app.tradeScreens->padPointer(app.camera.viewWidth, app.camera.viewHeight)) {
            putPointer(app, *p);
            return;
        }
    }
    if (s.at) {
        putPointer(app, middle(*s.at));
    }
}

void padPanelsIdle(App& app)
{
    auto& s = app.padPanels;
    if (!s.active && s.holding == Holding::None) {
        return;
    }
    if (s.holding == Holding::Item && app.inventory) {
        app.inventory->cancelDrag();
    } else if (s.holding == Holding::Bar) {
        app.barDrag.reset();
    }
    s.holding = Holding::None;
    if (app.tradeScreens) {
        app.tradeScreens->setPadNavigation(true); // (the windows' own again)
    }
    if (!tradeWindowsUp(app)) {
        s.inWindows = false; // (kept over their amount box: back to them after it)
    }
    s.clear();
}

bool padPanelsPress(App& app, const pad::PadEdge& edge)
{
    auto& s = app.padPanels;
    if (edge.down && pad::panelPress(edge.button) == PanelPress::Back && app.live && app.tradeScreens && app.tradeScreens->modalUp() &&
        app.pads.padInUse() && padGameInput(app) && !padHasMenus(app)) {
        // The amount box or a message over the game's windows (theirs to navigate): B puts it away,
        // as Escape does, rather than firing a slot under it.
        app.tradeScreens->escape();
        return true;
    }
    if (!edge.down || !s.active || !s.at || !allowed(app)) {
        return false;
    }
    // The modifier and its layer stay the game's (LT + View the book, LT + Up cure poison...).
    if (const auto mod = app.padRouter.modifier(); pad::modifierLayerPress(edge.button, mod, mod && app.pads.isDown(*mod))) {
        return false;
    }
    const PadButton b = edge.button;
    const PanelPress p = pad::panelPress(b);
    if (s.inWindows) {
        // The windows' navigation has the D-pad and A; on the inventory's screens B leaves (as
        // Escape) and the other buttons cast nothing under them.
        if (!app.tradeScreens || !app.tradeScreens->hasPad()) {
            // (just handed over: theirs from their next frame on; the press goes nowhere meanwhile)
            return pad::dpadDir(b).has_value() || p == PanelPress::Accept;
        }
        if (!s.still) {
            return false;
        }
        if (p == PanelPress::Back) {
            back(app);
            return true;
        }
        return p == PanelPress::Drop || p == PanelPress::Move || p == PanelPress::Secondary || p == PanelPress::PreviousPage;
    }
    if (const auto d = pad::dpadDir(b)) {
        step(app, *d);
        s.repeat.press(*d, SDL_GetTicks());
        return true;
    }
    if (p == PanelPress::None) {
        return false; // (View, Start, the triggers, the sticks' clicks: the game's)
    }
    const Spot cur = *s.at;
    if (!s.still) {
        // Only the D-pad's windows: A picks, B puts away the ones Escape puts away; the rest plays.
        if (p == PanelPress::Accept) {
            if (cur.panel == Panel::Summon) {
                clickHere(app, middle(cur));
            } else {
                pushClick(app, middle(cur));
            }
            log(app, "picked", cur);
            return true;
        }
        if (p == PanelPress::Back && cur.panel == Panel::Summon && cur.kind == 1) {
            app.summon->closeList();
            return true;
        }
        if (p == PanelPress::Back && cur.escapes) {
            pushEscape();
            return true;
        }
        return false;
    }
    if (barPress(app, cur, p)) {
        return true; // (the bars' slots, and a bar's spell held: dropped off the bar anywhere else)
    }
    switch (cur.panel) {
    case Panel::Inventory:
        inventoryPress(app, cur, p);
        break;
    case Panel::Book:
        bookPress(app, cur, p, b);
        break;
    case Panel::Bars:
    case Panel::Traps:
        break; // (barPress's)
    case Panel::Summon:
        if (p == PanelPress::Accept) {
            clickHere(app, middle(cur));
        } else if (p == PanelPress::Back) {
            if (cur.kind == 1) {
                app.summon->closeList();
            } else {
                back(app);
            }
        }
        break;
    case Panel::MapVote:
    case Panel::Mode:
        if (p == PanelPress::Accept) {
            pushClick(app, middle(cur));
            log(app, "picked", cur);
        } else if (p == PanelPress::Back) {
            if (cur.escapes) {
                pushEscape();
            } else {
                back(app);
            }
        }
        break;
    }
    return true; // (on the inventory's and the book's screens every face button and bumper is theirs)
}

bool padPanelsTakeBack(App& app, gui::NavInput in, SDL_FRect from)
{
    auto& s = app.padPanels;
    if (app.scriptPad) {
        SDL_Log("pad panels: the shop's windows hand back (%d) from %.0f, %.0f: active %d, in them %d, %zu spots", static_cast<int>(in), from.x,
                from.y, s.active, s.inWindows, s.spots.size());
    }
    if (!s.active || !s.inWindows || s.spots.empty()) {
        return false;
    }
    NavDir dir = NavDir::Up;
    switch (in) {
    case gui::NavInput::Up: dir = NavDir::Up; break;
    case gui::NavInput::Down: dir = NavDir::Down; break;
    case gui::NavInput::Left: dir = NavDir::Left; break;
    case gui::NavInput::Right: dir = NavDir::Right; break;
    default: return false;
    }
    fillBoxes(app);
    const auto next = pad::navPick(boxes, boxOf(from), dir);
    if (!next) {
        return false;
    }
    s.at = s.spots[*next];
    handToWindows(app, false);
    log(app, "back to", *s.at);
    return true;
}

bool padBarTipShown(const App& app)
{
    const auto& s = app.padPanels;
    if (!s.active || !s.still || s.inWindows || !s.at || !app.pads.padInUse() || !app.renderGui || !app.font) {
        return false;
    }
    return pad::barTipShown(onBarSlot(*s.at), slotSpell(app, *s.at) != 0, s.holding == Holding::Bar);
}

namespace {

/// The bar slot's tooltip as the game draws its own (drawTooltip: yellow lines 10 game pixels apart
/// on a darkened box), its spell's title over the pad's buttons for it (A Move, X Remove; held, A
/// Place, B Cancel), beside the slot: never over it, the other slots or the book.
void drawBarTip(App& app)
{
    const auto& s = app.padPanels;
    const Spot& cur = *s.at;
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const float k = Hud::scale(w, h);
    const bool holding = s.holding == Holding::Bar;
    Font& font = *app.font;
    constexpr float kFontHeight = 10; // (drawTooltip's line)
    const int size = font.lineSize(kFontHeight, k);
    // The buttons' line: their glyphs a little taller than the text, readable at any scale.
    const float glyphH = std::round(std::max(12 * k, pad::kMinPairHeight));
    const float buttonsH = glyphH + std::round(2 * k);
    std::string title;
    if (!holding && app.hud) {
        // The slot's own tooltip (Hud::tooltip: its spell's or ability's title).
        const auto& client = app.live->client();
        title = app.hud->tooltip(client.world(), client.netCode(), quickbarState(app), cur.box.cx(), cur.box.cy(), w, h);
    }
    const pad::BarTipPresses presses = pad::barTipPresses(holding);
    const auto word = [](PanelPress p) -> std::string_view {
        switch (p) {
        case PanelPress::Accept: return NOX_TR("game.pad.prompt.move", "Move");
        case PanelPress::Drop: return NOX_TR("game.pad.prompt.remove", "Remove");
        case PanelPress::Back: return NOX_TR("game.pad.prompt.cancel", "Cancel");
        default: return {};
        }
    };
    const std::array<std::string, 2> words{
        std::string(holding ? std::string_view(NOX_TR("game.pad.prompt.place", "Place")) : word(presses.first)),
        std::string(word(presses.second))};
    const pad::PromptStyle style = app.padConfig.settings.prompts;
    const std::array<pad::PromptLabels, 2> buttons{pad::PromptLabels{std::nullopt, app.pads.label(pad::panelButton(presses.first), style)},
                                                   pad::PromptLabels{std::nullopt, app.pads.label(pad::panelButton(presses.second), style)}};
    // Its size, in output pixels: the padding and lines drawTooltip's.
    const float pad2 = 2 * k;
    const float space = std::round(3 * k);
    const float between = std::round(8 * k);
    float buttonsW = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        buttonsW += pad::promptWidth(&font, buttons[i], glyphH) + space + font.measure(words[i], size) + (i == 0 ? between : 0);
    }
    const float titleW = title.empty() ? 0 : font.measure(title, size);
    const float boxW = std::ceil(std::max(titleW, buttonsW) + 2 * pad2);
    const float titleH = title.empty() ? 0 : kFontHeight * k;
    const float boxH = std::ceil(titleH + buttonsH + 2 * pad2);
    // Beside the slot, clear of the other spots and the book; a bar slot with the target nugget over
    // it (10 game pixels: it takes clicks too).
    const auto withNugget = [k](const Spot& sp) {
        NavBox b = sp.box;
        if (sp.panel == Panel::Bars) {
            b.y -= 10 * k;
            b.h += 10 * k;
        }
        return b;
    };
    boxes.clear();
    for (const auto& sp : s.spots) {
        if (!sp.sameAs(cur)) {
            boxes.push_back(withNugget(sp));
        }
    }
    if (app.book) {
        if (const auto b = app.book->bounds(w, h)) {
            boxes.push_back(boxOf(*b));
        }
    }
    if (!app.barsExpanded && !app.trapOpen) {
        const SDL_FRect t = Hud::spellSetTitleBox(w, h); // ("Spell Set n" over the bar)
        boxes.push_back(NavBox{t.x * k, t.y * k, t.w * k, t.h * k});
    }
    const NavBox at = pad::barTipPlace(withNugget(cur), boxW, boxH, boxes, NavBox{0, 0, static_cast<float>(w), static_cast<float>(h)}, std::round(3 * k));
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 128); // (drawTooltip's box)
    const SDL_FRect box = rectOf(at);
    SDL_RenderFillRect(app.renderer, &box);
    constexpr SDL_Color kYellow{255, 255, 0, 255};
    float y = at.y + pad2;
    if (!title.empty()) {
        font.draw(title, at.x + pad2, y, size, kYellow);
        y += titleH;
    }
    static const pad::GlyphInk ink{{1, 1, 1, 1}, {0.05f, 0.04f, 0.03f, 0.85f}, {0.55f, 0.52f, 0.48f, 1}}; // (the HUD's slot keys')
    const float cy = y + buttonsH / 2;
    float x = at.x + pad2;
    for (std::size_t i = 0; i < 2; ++i) {
        x += pad::drawPrompt(app.renderer, &font, buttons[i], x, cy, glyphH, ink) + space;
        font.draw(words[i], x, std::round(cy - font.lineHeight(size) / 2), size, kYellow);
        x += font.measure(words[i], size) + between;
    }
}

} // namespace

void drawPadPanels(App& app)
{
    const auto& s = app.padPanels;
    if (!s.active || s.inWindows || !s.at || !app.pads.padInUse() || !app.renderGui) {
        return;
    }
    // The menus' focus frame round the spot, a little inside it (pale steel while holding).
    const float unit = Hud::scale(app.camera.viewWidth, app.camera.viewHeight);
    SDL_FRect r = rectOf(s.at->box);
    const float in = std::round(unit * 2);
    r = {r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in};
    gui::drawFocusFrame(app.renderer, r, unit, s.holding != Holding::None ? gui::FocusLook::Holding : gui::FocusLook::Normal);
    if (padBarTipShown(app)) {
        drawBarTip(app);
    }
}

} // namespace noxsdl
