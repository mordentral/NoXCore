// (noxcpp) The game controller's snap cursor on the game's panels (pad_panels.cpp): the inventory,
// the spellbook, the bars and the trap tray beside them, the creature cage, the map vote's cards,
// the modes' windows. The D-pad (and, on the inventory and the book, the left stick) steps it
// between their spots (pad_panel_nav.hpp), the pointer sits on the one selected (so tooltips and
// the cursor follow it), the face buttons act there; a highlight marks it.
//
//   inventory  A uses / wears / takes off (a shop's sell or repair mode: asks the shop); X drops
//              (a stack asks how many); Y picks it up, Y (or A) again puts it down where the cursor
//              is (a cell, the doll, the secondary box); RB makes it the secondary weapon; A on the
//              secondary box swaps weapons; B puts a held item back, else closes (as Escape). The
//              tray scrolls as the cursor walks off its top or bottom row.
//   book       a slot button (X Y B RB LB as bound) on an entry or the page's icon puts it on that
//              slot of the set in use; A clicks (turns to the entry's page, casts the icon's spell,
//              the arrows, the tabs); LB / RB turn the page elsewhere; walking off its side too.
//   bars       (with the inventory or the book open) A or Y picks a slot's spell up and puts it
//              down on another (swapped), X empties it; the trap tray's slots likewise.
//   cage       A clicks a creature (its command list), a line of the list, the order button; B
//              puts the list away.
//   windows    the map vote's cards, a mode's windows (a campfire's spells, an altar's cards, the
//              Tide's level-up cards and results...): A picks; B where Escape puts it away. With only
//              these up the cursor has the D-pad alone: the stick still walks, the other buttons play.
// The shop's, the dialog's and the trade's windows are the window system's (gui_nav.hpp: their
// buttons, the stock's and the offers' cells); the cursor hands the pad to them walking off its
// spots their way, and they hand it back walking off theirs (TradeScreens::Actions::padLeave).
// While the cursor has the inventory or the book, the player stands, and the buttons above are
// the panels', not the slots' or A's; View, Start, the triggers, the sticks' clicks and the
// modifier's layer stay the game's. Nothing here runs without a pad in use.
#pragma once

#include "app.hpp"

namespace noxsdl {

/// What the snap cursor made of this frame (padSteer's): None (not up), Still (it has the stick: the
/// player stands, padSteer holds), Dpad (the D-pad alone: the stick walks; padPanelsPointer after
/// the play's steer).
enum class PanelFrame : std::uint8_t { None, Still, Dpad };
/// Each frame while the pad drives the game (padSteer, after its gates): the spots gathered, the
/// cursor stepped by a held direction, the pointer on it.
PanelFrame padPanelsFrame(App& app);
/// The pointer put back on the spot (after padPlaySteer moved it to the aim).
void padPanelsPointer(App& app);
/// The pad isn't driving the game: the cursor off, a move under way put back.
void padPanelsIdle(App& app);
/// A pad button's press, first of all (padEventFirst, before the menus' nav events): whether the
/// snap cursor took it.
bool padPanelsPress(App& app, const pad::PadEdge& edge);
/// The shop's, dialog's or trade's windows' pad focus leaving them `in`'s way from `from` (output
/// pixels): the cursor takes the pad there if it has a spot that way.
bool padPanelsTakeBack(App& app, gui::NavInput in, SDL_FRect from);
/// The cursor's highlight (drawPadOverlay's).
void drawPadPanels(App& app);

} // namespace noxsdl
