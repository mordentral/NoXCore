// The spell bars as played (app.hpp): casting from them, the sets, the trap tray, dragging
// spells, the award flying from the book, the book's events, the bars loaded and saved with
// the character, and the left button's way through the game's windows.
#include "app.hpp"

#include "noxcore/server/solo.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace noxsdl {

/// Tells the spell limits we've cast `spell`, from where we stand toward the cursor.
void noteCast(App& app, std::uint32_t spell)
{
    const auto& client = app.live->client();
    const auto* self = client.world().drawable(client.netCode());
    if (!self) {
        return;
    }
    const SDL_FPoint m = mousePos(app);
    app.spellLimits.cast(spell, clockFrame(), static_cast<float>(self->x), static_cast<float>(self->y),
                         app.camera.left() + m.x / app.camera.zoom, app.camera.top() + m.y / app.camera.zoom);
}

/// The class we play: the server's, as our own MSG_NEW_PLAYER told it (the stock client's own
/// too: a mode may make everyone a conjurer), else the character's.
std::uint8_t ownClass(const App& app)
{
    if (app.live) {
        if (const auto* me = ownPlayer(app.live->client())) {
            return me->playerClass;
        }
    }
    return app.profile.playerClass;
}

/// Whether a noxcpp mode's server made us multiclass (HeadlessClient::multiclass): one book and
/// bars of spells and abilities (an ability's id tagged, nox::client::kAbilityTag), mana.
bool multiclass(const App& app)
{
    return app.live && app.live->client().multiclass();
}

/// Whether we play a warrior (the bars hold abilities). Not a multiclass one: its bars hold
/// both, its meters a caster's.
bool isWarrior(const App& app)
{
    return ownClass(app) == 0 && !multiclass(app);
}

namespace {

/// A network game that isn't a quest (game flag 0x2000 without 0x1000), where everyone has every
/// spell: guiSpellSortList's and quickBarCreate's test (a solo game, 0x800 alone, isn't one).
/// (noxcpp: not where the server's mode owns the book, HeadlessClient::modeOwnsBook.)
bool everySpellKnown(const App& app)
{
    const auto& client = app.live->client();
    return nox::client::bookListsEverything(client.world().game().gameFlags, client.modeOwnsBook());
}

} // namespace

/// What the spellbook needs from the game this frame.
BookContext bookContext(const App& app)
{
    BookContext c;
    const auto& client = app.live->client();
    c.world = &client.world();
    c.playerClass = ownClass(app);
    c.quest = (client.world().game().gameFlags & 0x1000) != 0;
    c.allKnown = everySpellKnown(app);
    c.frame = gameFrame(app);
    c.multiclass = multiclass(app);
    c.modeOwnsBook = client.modeOwnsBook();
    return c;
}

/// The bars' state for the HUD.
QuickbarState quickbarState(const App& app)
{
    QuickbarState q;
    q.bars = &app.bars;
    q.expanded = app.barsExpanded;
    q.lastSlot = app.lastSlot;
    q.lastFrame = app.lastSlotFrame;
    q.bookOpen = app.book && app.book->isOpen();
    q.flash = SDL_GetTicks() - app.barFlashMs < 100 ? app.barFlash : -1;
    q.trapAvailable = app.trapArt;
    q.trapOpen = app.trapOpen;
    q.trapHit = SDL_GetTicks() - app.trapHitMs < 150;
    q.trapSlide = static_cast<float>(app.trapSlide);
    q.multiclass = multiclass(app);
    q.bottles = app.inventory ? &app.inventory->bottles() : nullptr;
    return q;
}

namespace {

/// The slot just used, lit for 10 frames (clientStoreLastButton).
void storeLastSlot(App& app, int slot)
{
    app.lastSlot = slot;
    app.lastSlotFrame = gameFrame(app);
}

} // namespace

/// Casts the spell (or uses the ability) in quickbar slot `i` of the set in use: its key
/// (client_invokeSpellSlot), the slot's flag saying whether it goes on us.
void castSlot(App& app, std::size_t i)
{
    auto& client = app.live->client();
    if (i >= 5) {
        return;
    }
    const auto slot = app.bars.current()[i];
    if (isWarrior(app)) {
        client.useAbility(static_cast<std::uint8_t>(slot.id));
    } else if (nox::client::isTaggedAbility(slot.id)) {
        client.useAbility(static_cast<std::uint8_t>(nox::client::untagged(slot.id))); // (multiclass)
    } else if (slot.id != 0) {
        const std::array<std::uint32_t, 1> spells{slot.id};
        client.castSpells(spells, slot.target);
        noteCast(app, slot.id);
    }
    storeLastSlot(app, static_cast<int>(i));
}

/// Makes `set` the one in use (clientUpdateButtonRow): not while all show.
void selectSpellSet(App& app, int set)
{
    // (A warrior has the one set: its bar has no arrows, QuickBarWarriorLeft.)
    if (set < 0 || set > 4 || app.barsExpanded || app.barsInverted || isWarrior(app)) {
        return;
    }
    app.bars.row = static_cast<std::uint8_t>(set);
    uiSound(app, 0x31E); // ChangeSpellbar
}

/// The next (+1) or previous (-1) spell set (client_spellSetNext, client_spellSetPrev).
void turnSpellSet(App& app, int by)
{
    if (app.barsExpanded) {
        return;
    }
    app.lastSlot = -1;
    selectSpellSet(app, (app.bars.row + by + 5) % 5);
}

/// All five sets shown at once, or back to the one (quickBarClose).
void expandBars(App& app, bool on)
{
    if (on == app.barsExpanded || (on && isWarrior(app))) { // (a warrior's one set: nothing to show)
        return;
    }
    if (on) {
        if (app.trapOpen) {
            app.trapOpen = false; // the tray goes
            uiSound(app, 0x31D); // TrapEditorClose
        }
        app.barsRowBefore = app.bars.row;
        app.bars.row = 4; // the bar itself shows the fifth
        uiSound(app, 0x31F); // ExpandSpellbar
    } else {
        app.bars.row = app.barsRowBefore;
        uiSound(app, 0x320); // CollapseSpellbar
    }
    app.barsExpanded = on;
}

namespace {

/// A game's string (Str_GetString with `file` as its source: its own label, then the file's).
std::string gameText(const App& app, std::string_view file, std::string_view id)
{
    if (auto t = tableText(app, std::string(file) + ":" + std::string(id))) {
        return std::move(*t);
    }
    return tableText(app, id).value_or(std::string(id));
}

/// Gui_PrintCenteredMessage's words, with NoCanDo.
void refuse(App& app, const std::string& words)
{
    centredMessage(app, words);
    uiSound(app, 0x39D); // NoCanDo
}

/// Whether the game is a quest (game flag 0x1000).
bool isQuest(const App& app)
{
    return (app.live->client().world().game().gameFlags & 0x1000) != 0;
}

/// Whether we know the Glyph (localPlayer.spellLevels[0x22]).
bool knowsGlyph(const App& app)
{
    const auto& levels = app.live->client().world().self().spellLevels;
    const auto it = levels.find(static_cast<std::uint8_t>(nox::client::kGlyph));
    return it != levels.end() && it->second != 0;
}

/// Whether a caster's bar has its trap / bomber end (quickBarCreate): with the Glyph known, or in a
/// network game that isn't a quest (everyone has every spell there; not a solo game).
bool trapUsable(const App& app)
{
    return !isWarrior(app) && (knowsGlyph(app) || everySpellKnown(app));
}

/// The trap tray open or shut: opening it closes the five sets' view.
void openTrapTray(App& app, bool open)
{
    if (open == app.trapOpen || (open && !app.trapArt)) {
        return;
    }
    if (open) {
        expandBars(app, false);
    }
    app.trapOpen = open;
    uiSound(app, open ? 0x31C : 0x31D); // TrapEditorOpen / TrapEditorClose
}

/// The next (+1) or previous (-1) trap set (client_trapSetNext, client_trapSetPrev).
void turnTrapSet(App& app, int by)
{
    app.bars.trapRow = static_cast<std::uint8_t>((app.bars.trapRow + by + 3) % 3);
    uiSound(app, 0x31E); // ChangeSpellbar
}

} // namespace

/// Builds the trap (a conjurer: the bomber) of the trap set in use (client_buildTrap): its spells
/// and SPELL_GLYPH in one MSG_TRY_SPELL, with the last cast's target flag. An empty set says
/// TrapError when we know the Glyph.
void buildTrap(App& app)
{
    if (isWarrior(app)) {
        return;
    }
    const auto spells = app.bars.trapCast();
    if (spells.empty()) {
        if (knowsGlyph(app) || everySpellKnown(app)) {
            refuse(app, gameText(app, "guispell.c", "TrapError"));
        }
        return;
    }
    app.live->client().castSpells(spells, app.lastTargetFlag);
}

/// The trap / bomber end each frame: shown once a trap can be laid; learning the Glyph in a quest
/// (with the award shown) slides it off the bottom and back on changed (quickbarAddTrap, quickbarDrawFn).
void updateTrapBar(App& app)
{
    const auto frame = gameFrame(app);
    if (app.trapSlideDir != 0) {
        // A game frame at a time: down a pixel a frame until past the bar's 74, then back up.
        for (; app.trapSlideFrame < frame; ++app.trapSlideFrame) {
            app.trapSlide += app.trapSlideDir;
            if (app.trapSlideDir > 0 && app.trapSlide > 74) {
                app.trapArt = true; // off the screen's bottom: changed, and back up
                app.trapSlideDir = -1;
            } else if (app.trapSlideDir < 0 && app.trapSlide <= 0) {
                app.trapSlide = 0;
                app.trapSlideDir = 0;
                break;
            }
        }
        return;
    }
    if (!app.trapArt && trapUsable(app)) {
        app.trapArt = true;
    } else if (app.trapArt && !trapUsable(app)) {
        app.trapArt = false;
        app.trapOpen = false;
    }
}

namespace {

/// A spell clicked (guiSpellSetCursor): one that goes on its caster (the
/// slot's flag) is cast at once; any other waits for a click where it's to go.
void clickSpell(App& app, std::uint32_t spell, bool onSelf)
{
    if (spell == 0) {
        return;
    }
    uiSound(app, 0x2FE); // ButtonPress
    if (onSelf && !(nox::client::spellFlags(app.things, spell) & 0x2000)) {
        const std::array<std::uint32_t, 1> spells{spell};
        app.live->client().castSpells(spells, true);
        app.lastTargetFlag = true;
        noteCast(app, spell);
        return;
    }
    app.armed = App::Armed{spell, false};
}

/// An ability clicked: War Cry, Tread Lightly and Eye of the Wolf go at once (the
/// ability table's flag); Berserker Charge and the harpoon wait to be aimed.
void clickAbility(App& app, std::uint32_t ability)
{
    if (ability == 0) {
        return;
    }
    uiSound(app, 0x2FE);
    if (ability == 2 || ability == 4 || ability == 5) {
        app.live->client().useAbility(static_cast<std::uint8_t>(ability));
        return;
    }
    app.armed = App::Armed{ability, true};
}

/// Lets `what` go at output pixel (x, y) (bookSpellDrop): into the slot there
/// among the showing sets, if any. From the book a spell is lit if it goes on its caster; from
/// a slot it keeps its flag. Returns what the slot held.
std::optional<nox::client::BarSlot> dropOnBars(App& app, nox::client::BarSlot what, bool fromBook, float x, float y,
                                               bool fromTray = false)
{
    if (!app.hud) {
        return std::nullopt;
    }
    const auto q = quickbarState(app);
    if (!fromTray) {
        if (const auto t = app.hud->trapSlotAt(q, x, y, app.camera.viewWidth, app.camera.viewHeight)) {
            // Into the tray (spellPutInBox): a spell traps may hold, once a set.
            auto& row = app.bars.currentTrap();
            if (!nox::client::canTrap(app.things, what.id, isQuest(app))) {
                refuse(app, gameText(app, "guispell.c", "RestrictedTrapSpell"));
            } else if (std::ranges::any_of(row, [&](const nox::client::BarSlot& s) { return s.id == what.id; })) {
                refuse(app, gameText(app, "guispell.c", "OneSpellPerTrap"));
            } else {
                row[static_cast<std::size_t>(*t)].id = what.id; // (its flag as it was)
                uiSound(app, 0x31A); // SpellDrop
            }
            return std::nullopt; // (what a slot dragged there held stays where it was)
        }
    }
    const auto at = app.hud->slotAt(q, x, y, app.camera.viewWidth, app.camera.viewHeight);
    if (!at) {
        return std::nullopt;
    }
    auto& slot = app.bars.rows[static_cast<std::size_t>(at->first)][static_cast<std::size_t>(at->second)];
    const auto old = slot;
    if (fromBook) {
        app.bars.place(at->first, at->second, what.id, app.things);
        if (isWarrior(app)) {
            slot.target = false;
        }
    } else {
        slot = what;
    }
    uiSound(app, 0x31A); // SpellDrop (spellKeyPackSetSpell)
    return old;
}

/// The left button on the spell bars (quickBarWnd and its buttons): pressing a
/// slot picks its spell up; letting go on another slot swaps them, off the bar empties it, on
/// the same slot clicks it. Returns whether the bars took it.
bool barsMouse(App& app, bool down, float x, float y)
{
    if (!app.hud) {
        return false;
    }
    auto& client = app.live->client();
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const auto q = quickbarState(app);
    if (!down) {
        if (!app.barDrag) {
            return false;
        }
        const auto drag = *app.barDrag;
        app.barDrag.reset();
        if (drag.set < 0) {  // set -1: from the trap tray
            // From the tray (quickBarWnd for its box): clicked, nothing; to another of its slots,
            // swapped; to a bar slot, swapped with it unchecked; off the bar, gone.
            auto& from = app.bars.currentTrap()[static_cast<std::size_t>(drag.slot)];
            if (const auto t = app.hud->trapSlotAt(q, x, y, w, h)) {
                if (*t != drag.slot) {
                    std::swap(from, app.bars.currentTrap()[static_cast<std::size_t>(*t)]);
                    uiSound(app, 0x31A);
                }
                return true;
            }
            if (const auto old = dropOnBars(app, drag.what, false, x, y, true)) {
                from = *old;
            } else if (!Hud::inBarArea(x, y, w, h)) {
                from = {};
            }
            return true;
        }
        auto& from = app.bars.rows[static_cast<std::size_t>(drag.set)][static_cast<std::size_t>(drag.slot)];
        const auto at = app.hud->slotAt(q, x, y, w, h);
        if (at && at->first == drag.set && at->second == drag.slot) {
            // A click on it.
            if (isWarrior(app) || nox::client::isTaggedAbility(from.id)) {
                clickAbility(app, nox::client::untagged(from.id)); // (a multiclass ability's tagged)
                storeLastSlot(app, drag.slot);
            } else if (Hud::spellAllowed(client.world(), from.id)) {
                clickSpell(app, from.id, from.target);
                storeLastSlot(app, drag.slot);
            }
            return true;
        }
        if (at && at->first == drag.set) {
            // Another slot of its set: swapped (clientSwapQuickbarKeys).
            std::swap(from, app.bars.rows[static_cast<std::size_t>(at->first)][static_cast<std::size_t>(at->second)]);
            uiSound(app, 0x31A);
            return true;
        }
        if (const auto old = dropOnBars(app, drag.what, false, x, y)) {
            from = *old; // another set's slot: what it held comes back here
        } else if (!Hud::inBarArea(x, y, w, h)) {
            from = {}; // dragged off the bar
        }
        return true;
    }
    const auto hit = app.hud->hit(client.world(), client.netCode(), q, x, y, w, h);
    if (!hit) {
        return false;
    }
    switch (hit->kind) {
    case HudHit::Kind::Slot: {
        const auto& slot = app.bars.rows[static_cast<std::size_t>(hit->set)][static_cast<std::size_t>(hit->index)];
        if (slot.id != 0 && (isWarrior(app) || Hud::spellAllowed(client.world(), slot.id))) {
            app.barDrag = App::BarDrag{hit->set, hit->index, slot};
            uiSound(app, 0x319); // SpellPickup
        }
        break;
    }
    case HudHit::Kind::TrapSlot: {
        const auto& slot = app.bars.currentTrap()[static_cast<std::size_t>(hit->index)];
        if (slot.id != 0 && Hud::spellAllowed(client.world(), slot.id)) {
            app.barDrag = App::BarDrag{-1, hit->index, slot};
            uiSound(app, 0x319); // SpellPickup
        }
        break;
    }
    case HudHit::Kind::TrapUp:
    case HudHit::Kind::TrapDown:
        app.barFlash = hit->kind == HudHit::Kind::TrapUp ? 3 : 4;  // barFlash 3 / 4: the trap set's up / down button lights
        app.barFlashMs = SDL_GetTicks();
        turnTrapSet(app, hit->kind == HudHit::Kind::TrapUp ? -1 : 1);
        break;
    case HudHit::Kind::TrapToggle:
        openTrapTray(app, !app.trapOpen); // (quickbarTrapProc)
        break;
    case HudHit::Kind::TrapBuild: {
        // quickbarTrapButtonProc: a wizard's trap with an at-location spell (flag 8) waits for a click where
        // it's to go; anything else is built at once.
        app.trapHitMs = SDL_GetTicks();
        const auto& row = app.bars.currentTrap();
        const bool placed = ownClass(app) == 1 && std::ranges::any_of(row, [&](const nox::client::BarSlot& s) {
                                return s.id != 0 && (nox::client::spellFlags(app.things, s.id) & 8);
                            });
        if (placed) {
            app.armed = App::Armed{0, false, true};
        } else {
            buildTrap(app);
        }
        break;
    }
    case HudHit::Kind::Target: {
        // The nugget's corner: on us or not; never for one that can't change.
        auto& slot = app.bars.rows[static_cast<std::size_t>(hit->set)][static_cast<std::size_t>(hit->index)];
        if (slot.id == 0) {
            break;
        }
        if (nox::client::spellFlags(app.things, slot.id) & 0x200400) {  // those spell flags: a target that can't change
            uiSound(app, 0x39D); // NoCanDo
        } else {
            slot.target = !slot.target;
            uiSound(app, 0x399); // ShellClick
        }
        break;
    }
    case HudHit::Kind::SetUp:
    case HudHit::Kind::SetDown:
    case HudHit::Kind::SetMax:
        // quickbarTrapUpDownProc; the button lights a moment.
        app.barFlash = hit->kind == HudHit::Kind::SetUp ? 0 : hit->kind == HudHit::Kind::SetDown ? 1 : 2;
        app.barFlashMs = SDL_GetTicks();
        if (hit->kind == HudHit::Kind::SetMax) {
            expandBars(app, !app.barsExpanded);
        } else {
            turnSpellSet(app, hit->kind == HudHit::Kind::SetUp ? -1 : 1);
        }
        break;
    case HudHit::Kind::Tube:
        app.hud->toggleBars();
        uiSound(app, 0x385); // ManaClick
        break;
    case HudHit::Kind::Potion:
        drinkPotion(app, hit->index);
        break;
    case HudHit::Kind::Spellbook:
        if (app.book) {
            app.book->toggle(bookContext(app)); // (quickbarButtonBookWnd)
        }
        break;
    case HudHit::Kind::Flag:
    case HudHit::Kind::Panel:
        break;
    }
    return true;
}

} // namespace

/// Uses what a potion slot holds (0 health, 1 mana, 2 cure poison; quickHealthPotion and the
/// others, a slot's click): the first item of the first stack of the slot's type, if there's one.
/// The health slot holds food when there's no red potion (bottle_slots.hpp). A warrior has no
/// mana slot (guiHealthManaInit makes it for the casters only).
void drinkPotion(App& app, int kind)
{
    if (!app.inventory || (kind == BottleSlots::kMana && isWarrior(app))) {
        return;
    }
    if (const auto item = app.inventory->bottleItem(kind)) {
        app.live->client().use(*item);
    }
}

namespace {

/// The spellbook's hands on the rest of the client.
Spellbook::Actions bookActions(App& app)
{
    Spellbook::Actions a;
    a.drop = [&app](std::uint32_t id, float x, float y) { dropOnBars(app, {id, false}, true, x, y); };
    a.click = [&app](std::uint32_t id) {
        if (app.armed) {
            app.armed.reset(); // clicked again: put down (bookWndFn)
        } else if (isWarrior(app) || nox::client::isTaggedAbility(id)) {
            clickAbility(app, nox::client::untagged(id)); // (a multiclass ability's tagged)
        } else {
            clickSpell(app, id, (nox::client::spellFlags(app.things, id) & 0x600) != 0);
        }
    };
    return a;
}

/// The flight's start (bookFillAll: the solo moment (0, kind)) or landing
/// (Game_SetPaused(0)) on the solo game we host (solo.hpp).
void hostBookFlight(App& app, bool begins, int kind)
{
    if (!app.host) {
        return;
    }
    const bool paused = app.host->withGame([&](nox::server::GameHost& game) {
        auto& server = game.server();
        if (begins) {
            server.bookFlightBegins(kind);
        } else {
            server.bookFlightLands();
        }
        return (server.world().gameFlags & nox::server::GameFlag::Paused) != 0;
    });
    SDL_Log("book flight %s: the game %s", begins ? "begins" : "lands", paused ? "paused" : "running");
}

/// An award put in at once, 50 sparks from `from` to `to` by fiftieths, the spell
/// or ability in its slot (the book's put-in), then the set before chosen again, and in a
/// solo game we host the moment over, unpaused.
void landAtOnce(App& app, std::uint32_t id, bool ability, int row, int slot, int before, SDL_FPoint from, SDL_FPoint to, bool solo)
{
    auto& p = app.book->particles;
    int x = static_cast<int>(from.x);
    int y = static_cast<int>(from.y);
    const int dx = static_cast<int>(to.x) - x;
    const int dy = static_cast<int>(to.y) - y;
    for (int n = 0; n < 50; ++n) {
        // (the size drawn first, then the y offset, then the x offset)
        const int size = p.rand(3, 4);
        const int py = y - p.rand(0, 0x1E);
        const int px = x + p.rand(0, 0x1E);
        p.add(ability ? 3 : 0, px, py, 0, 0, 1, size, 0, 0, true);
        x += dx / 50;
        y += dy / 50;
    }
    if (ability) {
        app.bars.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(slot)].id = id;
        uiSound(app, 0x31A);
    } else {
        app.bars.place(row, slot, id, app.things);
        uiSound(app, 0x31A); // SpellDrop
    }
    selectSpellSet(app, before);
    if (solo) {
        hostBookFlight(app, false, 0);
    }
}

} // namespace

/// An award's spell or ability to the bars' first empty slot (bookFillAll), as
/// a network game does it, at once: the set with the slot chosen, sparks along the
/// way from the book's icon, the spell put in, the set before chosen again.
void fillBars(App& app, std::uint32_t id, bool ability)
{
    // (bookFillAll's first test: nothing while an award flies.)
    if (app.flight) {
        return;
    }
    // Not a general spell (flags 0x1000 / 0x4000 / 0x10000: see the revoke in bookEvents), nor one already on the bars.
    if ((!ability && (nox::client::spellFlags(app.things, id) & 0x15000)) || app.bars.contains(id)) {
        return;
    }
    expandBars(app, false);
    const int before = app.bars.row;
    std::optional<std::pair<int, int>> at;
    if (ability) {
        if (const auto i = app.bars.firstEmptyInRow()) {
            at = std::pair{before, *i};
        }
    } else {
        at = app.bars.firstEmpty();
        if (at && at->first != before) {
            selectSpellSet(app, at->first);
        }
    }
    if (!at && app.forceAwardFlight) {
        at = std::pair{before, 4}; // (--award-flight on full bars: the set's last slot, for a test)
    }
    if (!at) {
        return;
    }
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const float s = Hud::scale(w, h);
    const SDL_FPoint from = app.book->iconPosition(static_cast<int>(static_cast<float>(h) / s));
    const SDL_FPoint to = Hud::slotPosition(w, h, at->first, at->second, false, at->first);
    const bool solo = soloGame(app);
    if (solo) {
        hostBookFlight(app, true, ability ? 3 : 2); // (the solo moment (0, kind): the game paused)
    }
    // (the in-game menu open (gui_xxx_check) or a save pending
    // (Game_IsSavePending) lands it at once, as a network game does)
    const bool savePending =
        app.host && app.host->withGame([](nox::server::GameHost& game) { return game.server().world().saveRequest.pending; });
    if ((solo && !(app.quitMenu && app.quitMenu->isOpen()) && !savePending) || app.forceAwardFlight) {
        // A solo game flies it (bookFillAll): a cubic Hermite from the book's icon to the slot with
        // tangents (400, -500) and (350, 400), in 19 pieces.
        App::AwardFlight f;
        f.id = id;
        f.ability = ability;
        f.row = at->first;
        f.slot = at->second;
        f.before = before;
        f.pos = from;
        f.target = to;
        const float t0x = 400, t0y = -500, t1x = 350, t1y = 400;
        // The cubic's coefficients: p(t) = a t^3 + b t^2 + t0 t + from, so p(0) is `from` and p(1) is `to`.
        const float ax = 2 * from.x - 2 * to.x + t0x + t1x, ay = 2 * from.y - 2 * to.y + t0y + t1y;
        const float bx = -3 * from.x + 3 * to.x - 2 * t0x - t1x, by = -3 * from.y + 3 * to.y - 2 * t0y - t1y;
        SDL_FPoint prev = from;
        for (int i = 1; i <= 19; ++i) {
            const float t = std::min(1.0f, static_cast<float>(i) / 19);
            const SDL_FPoint next{std::round(ax * t * t * t + bx * t * t + t0x * t + from.x),
                                  std::round(ay * t * t * t + by * t * t + t0y * t + from.y)};
            f.path.emplace_back(prev, next);
            prev = next;
        }
        app.flight = std::move(f);
        return; // (the set stays chosen until it lands)
    }
    landAtOnce(app, id, ability, at->first, at->second, before, from, to, solo);
}

/// A flying award landed at once (Escape, consoleEsc, or a save):
/// sparks from where its icon is now (which
/// bookDrawFn moves) to its slot, the spell put in (the book's put-in).
void landFlightAtOnce(App& app)
{
    if (!app.flight || !app.book) {
        return;
    }
    const App::AwardFlight f = std::move(*app.flight);
    app.flight.reset();
    landAtOnce(app, f.id, f.ability, f.row, f.slot, f.before, f.pos, f.target, soloGame(app));
}

namespace {

/// A flying award put in its slot (the book's put-in) and the flight over (the
/// set before chosen again, the moment over on our host, unpaused).
void landFlight(App& app)
{
    if (!app.flight) {
        return;
    }
    const auto f = *app.flight;
    if (f.ability) {
        app.bars.rows[static_cast<std::size_t>(f.row)][static_cast<std::size_t>(f.slot)].id = f.id;
    } else {
        app.bars.place(f.row, f.slot, f.id, app.things);
    }
    uiSound(app, 0x31A);
    selectSpellSet(app, f.before);
    app.flight.reset();
    hostBookFlight(app, false, 0);
}

} // namespace

/// A flying award, a game frame on (bookDrawFn); lands it at the end (the book's put-in).
void stepFlight(App& app)
{
    if (!app.flight || !app.book) {
        return;
    }
    auto& f = *app.flight;
    auto& p = app.book->particles;
    const int kind = f.ability ? 3 : 0;
    if (++f.frames < 60) {
        return; // two seconds with the book open on it first
    }
    if (!f.burst) {
        f.burst = true;
        for (int n = 0; n < 50; ++n) {
            const int timer = p.rand(3, 6);
            const int size = p.rand(2, 5);
            const int vy = p.rand(-10, -1);
            const int vx = p.rand(-10, 10);
            const int y = static_cast<int>(f.pos.y) + p.rand(0, 0x1E);
            p.add(kind, static_cast<int>(f.pos.x) + p.rand(0, 0x1E), y, vx, vy, 1, size, timer);
        }
        uiSound(app, 0x31B); // SpellPopOffBook
        const float w = static_cast<float>(app.camera.viewWidth) / Hud::scale(app.camera.viewWidth, app.camera.viewHeight);
        // The icon's speed in game pixels a frame, by the screen's width: 6, 8 or 10.
        const float speed = w < 1000 ? (w > 749 ? 8.0f : 6.0f) : 10.0f;
        const auto& [a, b] = f.path.front();
        const float dx = b.x - a.x, dy = b.y - a.y, len = std::max(0.001f, std::hypot(dx, dy));
        f.step = {dx / len * speed, dy / len * speed};
    }
    for (int n = 0; n < 2; ++n) {
        const int timer = p.rand(2, 4);
        const int size = p.rand(1, 2);
        const int y = static_cast<int>(f.pos.y) + p.rand(0, 0x1E);
        p.add(kind, static_cast<int>(f.pos.x) + p.rand(0, 0x1E), y, 0, 0, 0, size, timer, 1, true);
    }
    f.pos.x += f.step.x;
    f.pos.y += f.step.y;
    // Landed once past the target on both axes; a piece is done when x passes its end.
    bool landed = f.pos.x >= f.target.x && f.pos.y >= f.target.y;
    if (!landed && f.pos.x > f.path[f.piece].second.x) {
        // On to the next piece, at the same speed.
        if (++f.piece >= f.path.size()) {
            landed = true;
        } else {
            const auto& [a, b] = f.path[f.piece];
            const float speed = std::hypot(f.step.x, f.step.y);
            const float dx = b.x - a.x, dy = b.y - a.y, len = std::max(0.001f, std::hypot(dx, dy));
            f.step = {dx / len * speed, dy / len * speed};
        }
    }
    if (landed) {
        landFlight(app);
    }
}

/// The flying award's icon (bookDrawFn's image), in game pixels.
void drawFlight(App& app)
{
    if (!app.flight || app.flight->frames < 60 || !app.hud) {
        return;
    }
    const auto ic = app.hud->icon(app.flight->id, app.flight->ability);
    const Sprite* sp = ic ? app.sprites->get(*ic) : nullptr;
    if (!sp) {
        return;
    }
    const float s = Hud::scale(app.camera.viewWidth, app.camera.viewHeight);
    const SDL_FRect dst{(app.flight->pos.x + sp->offsetX) * s, (app.flight->pos.y + sp->offsetY) * s, sp->width * s,
                        sp->height * s};
    app.sprites->render(*sp, nullptr, dst);
}

/// The spellbook's awards and revokes (World::takeBookEvents), and what they do to the bars.
void bookEvents(App& app)
{
    auto& client = app.live->client();
    const auto events = client.takeBookEvents();
    if (!app.book) {
        return;
    }
    using Kind = nox::client::BookEvent::Kind;
    for (const auto& e : events) {
        switch (e.kind) {
        case Kind::RevokeSpell: {
            // Cli_RevokeSpell: the spell, and a general one's linked spells, off the bars.
            app.bars.remove(e.id);
            const std::uint32_t flags = nox::client::spellFlags(app.things, e.id);
            // A general spell (0x1000, 0x4000, 0x10000) marks its linked spells with the next bit up (0x2000, 0x8000,
            // 0x20000); every spell id is checked.
            const std::uint32_t linked = flags & 0x1000 ? 0x2000 : flags & 0x4000 ? 0x8000 : flags & 0x10000 ? 0x20000 : 0;
            for (std::uint32_t id = 1; linked && id < 0x89; ++id) {  // 0x89: one past the last spell id
                if (nox::client::spellFlags(app.things, id) & linked) {
                    app.bars.remove(id);
                }
            }
            break;
        }
        case Kind::RevokeCreature:
            // A creature's guide revoked: its summon spell off the bars, and its group's.
            app.bars.remove(nox::things::guideSummonSpell(e.id));
            for (const int c : nox::things::guideGroup(e.id)) {
                app.bars.remove(nox::things::guideSummonSpell(c));
            }
            break;
        case Kind::RevokeAbility:
            app.bars.remove(multiclass(app) ? nox::client::kAbilityTag | static_cast<std::uint32_t>(e.id) : static_cast<std::uint32_t>(e.id));
            break;
        case Kind::Spell:
            // The Glyph learned (quickbarAddTrap): first time and shown, the bar's
            // end slides off and back as the trap or bomber, with sparks; else it just changes.
            if (e.id == nox::client::kGlyph && !app.trapArt && trapUsable(app)) {
                const auto& levels = client.world().self().spellLevels;
                const auto lv = levels.find(static_cast<std::uint8_t>(nox::client::kGlyph));
                if (e.show && lv != levels.end() && lv->second == 1) {
                    app.trapSlideDir = 1;
                    app.trapSlideFrame = gameFrame(app);
                    const float s = Hud::scale(app.camera.viewWidth, app.camera.viewHeight);
                    const int H = static_cast<int>(static_cast<float>(app.camera.viewHeight) / s);
                    const int W = static_cast<int>(static_cast<float>(app.camera.viewWidth) / s);
                    // The trap end of the 320-wide quickbar: 0x104 in from its left, its top 74 up from the bottom.
                    const int rx = (W - 320) / 2 + 0x104;
                    const int ry = H - 74;
                    auto& p = app.book->particles;
                    for (int n = 0; n < 50; ++n) {
                        const int x = p.rand(0, 0x14) + 10 + rx;
                        const int y = p.rand(0, 0x14) + 10 + ry;
                        p.add(0, x, y, p.rand(-5, 5), p.rand(-0x14, -5), 1, p.rand(3, 6), p.rand(4, 6));
                    }
                } else {
                    app.trapArt = true;
                }
            }
            break;
        default:
            break;
        }
        // Then the book itself: an award it says to fill goes onto the bars.
        if (const auto fill = app.book->event(e, bookContext(app))) {
            fillBars(app, *fill, e.kind == Kind::Ability);
        }
    }
}

/// The character's spell bars from its .plr's Gui section (Plr_LoadClientPlayerData),
/// or, without one, a new character's: one of the class's starting sets.
void loadBars(App& app)
{
    // (Our class's: the character's, or the server's once it's told another. A character's bars
    // of another class aren't taken: a class's starting set instead.)
    const auto cls = static_cast<nox::player::CharacterClass>(ownClass(app));
    app.barsClass = ownClass(app);
    app.barsMulticlass = multiclass(app);
    app.barsExpanded = false;
    app.lastSlot = -1;
    app.armed.reset();
    app.barDrag.reset();
    bool loaded = false;
    if (!app.characterFile.empty()) {
        try {
            const auto file = nox::player::loadPlayerFile(app.characterFile);
            if (const auto* raw = file.find(nox::player::SectionId::Gui)) {
                const auto gui = nox::player::Gui::parse(raw->data);
                if (gui.characterClass == cls) {
                    app.bars = nox::client::SpellBars::fromGui(gui);
                    SDL_Log("spell bars: from %s", app.characterFile.string().c_str());
                    loaded = true;
                }
            }
        } catch (const std::exception& ex) {
            SDL_Log("spell bars: reading %s: %s", app.characterFile.string().c_str(), ex.what());
        }
    }
    if (!loaded) {
        const int presets = nox::client::SpellBars::presetCount(cls);
        const int preset = nox::util::uniform_int_distribution<int>(0, std::max(presets - 1, 0))(app.rng);
        app.bars = nox::client::SpellBars::defaults(cls, preset, app.things);
    }
    // noxcpp's multiclass: a warrior's abilities tagged (its bars hold spells too).
    if (app.barsMulticlass && cls == nox::player::CharacterClass::Warrior) {
        for (auto& row : app.bars.rows) {
            for (auto& slot : row) {
                if (slot.id != 0) {
                    slot.id |= nox::client::kAbilityTag;
                }
            }
        }
    }
}

/// The bars back into the character's .plr when the game is left (the client's sections):
/// its Gui section replaced, the rest as it was.
void saveBars(App& app)
{
    expandBars(app, false);
    // (Game_FinishQuit: only a network game's, not a quest's, under game flag 0x2000
    // without 0x1000; a solo game's .plr is the server's save's.)
    if (app.characterFile.empty() || (app.live && (soloGame(app) || (gameFlags(app) & 0x1000u)))) {
        return;
    }
    if (ownClass(app) != app.profile.playerClass || app.barsMulticlass) { // (a class the server gave, or multiclass bars: not the character's)
        return;
    }
    try {
        auto file = nox::player::loadPlayerFile(app.characterFile);
        const auto cls = static_cast<nox::player::CharacterClass>(app.profile.playerClass);
        const auto data = app.bars.toGui(cls).serialize();
        const auto it = std::ranges::find_if(file.sections, [](const nox::player::RawSection& r) {
            return r.id == static_cast<std::uint32_t>(nox::player::SectionId::Gui);
        });
        if (it != file.sections.end()) {
            if (nox::player::Gui::parse(it->data).characterClass != cls) {
                SDL_Log("spell bars: %s is another class's; not saved", app.characterFile.string().c_str());
                return;
            }
            it->data = data;
        } else {
            file.sections.insert(file.sections.begin(), {static_cast<std::uint32_t>(nox::player::SectionId::Gui), data});
        }
        nox::player::savePlayerFile(app.characterFile, file);
        SDL_Log("spell bars: saved to %s", app.characterFile.string().c_str());
    } catch (const std::exception& ex) {
        SDL_Log("spell bars: saving %s: %s", app.characterFile.string().c_str(), ex.what());
    }
}

namespace {

/// A left press or release for the inventory: returns whether it took it.
bool inventoryMouse(App& app, bool down, float x, float y)
{
    if (!app.inventory) {
        return false;
    }
    auto& client = app.live->client();
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const auto now = static_cast<std::uint32_t>(SDL_GetTicks());
    if (down) {
        return app.inventory->mouseDown(x, y, w, h, client.world(), now);
    }
    Inventory::Actions actions;
    actions.use = [&client](std::uint16_t nc) { client.use(nc); };
    actions.equip = [&client](std::uint16_t nc) { client.equip(nc); };
    actions.unequip = [&client](std::uint16_t nc) { client.unequip(nc); };
    actions.drop = [&client](std::uint16_t nc, int wx, int wy) { client.dropAt(nc, wx, wy); };
    actions.secondary = [&client](std::uint16_t nc) { client.setSecondaryWeapon(nc); };
    actions.putAwayWeapon = [&client](std::uint16_t nc) { client.putAwayWeapon(nc); };
    actions.weaponBit = [&client](std::uint16_t typeId) { return client.weaponBit(typeId); };
    const auto toWorld = [&app](float sx, float sy) {
        return SDL_Point{static_cast<int>(app.camera.left() + sx / app.camera.zoom),
                         static_cast<int>(app.camera.top() + sy / app.camera.zoom)};
    };
    return app.inventory->mouseUp(x, y, w, h, client.world(), now, toWorld, actions);
}

} // namespace

/// The left button in a game, to what's on top first: a spell waiting for its target, the
/// book, the inventory, the bars. Whether one of them took it.
bool leftMouse(App& app, bool down, float x, float y)
{
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    if (app.armed) {
        // The target cursor's click (quickBarWnd, bookWndFn): let go on a slot or the book it's put
        // down; anywhere else the spell (or aimed ability) goes there.
        if (!down) {
            const auto a = *app.armed;
            app.armed.reset();
            const auto& world = app.live->client().world();
            const auto hit = app.hud ? app.hud->hit(world, app.live->client().netCode(), quickbarState(app), x, y, w, h)
                                     : std::nullopt;
            const bool onGui = (app.hud && app.hud->slotAt(quickbarState(app), x, y, w, h)) ||
                               (app.book && app.book->contains(x, y, w, h)) ||
                               (a.trap && hit && hit->kind == HudHit::Kind::TrapBuild);
            if (!onGui) {
                if (const auto v = app.view3d.cursorMouse(app.camera)) {
                    x = v->x; // (the 3D view: the crosshair's point)
                    y = v->y;
                }
                auto& client = app.live->client();
                client.setCursor(static_cast<int>(app.camera.left() + x / app.camera.zoom),
                                 static_cast<int>(app.camera.top() + y / app.camera.zoom));
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
        }
        return true;
    }
    if (app.summon) {
        // The cage and its command list (modal while it's up).
        auto& client = app.live->client();
        const auto command = [&client](std::uint16_t nc, std::uint8_t c) { client.commandCreature(nc, c); };
        if (down ? app.summon->mouseDown(x, y, w, h) : app.summon->mouseUp(x, y, w, h, client.world(), command)) {
            return true;
        }
    }
    if (app.book) {
        const auto ctx = bookContext(app);
        if (down ? app.book->mouseDown(x, y, w, h, ctx) : app.book->mouseUp(x, y, w, h, ctx, bookActions(app))) {
            return true;
        }
    }
    if (inventoryMouse(app, down, x, y)) {
        return true;
    }
    return barsMouse(app, down, x, y);
}

} // namespace noxsdl
