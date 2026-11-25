// (noxcpp) The game controller in a game (pad_play.cpp): its pointer behind mousePos (the aim
// point), the target and the aim (pad_aim.hpp) fed from the frame's drawables, casts aimed before
// they go, A on what's in front, and their draw. The pad steers no free point of its own: the
// sticks give ways, the aim point follows from them.
//
// The frame: padSteer (sendGameInput's first) calls padPlaySteer while the pad drives the game:
// last frame's seen enemies give the target, the cursor point and facing are set and sent, the
// pointer put on the aim point, and liveSprites asked to gather this frame's (padWants / padNoteSeen).
// Nothing runs without a pad in use: padPlayIdle drops it all.
#pragma once

#include "app.hpp"

#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

/// padSteer's while the pad drives the game (padGameInput, the 2D view), `from` our spot as the
/// server last put us: the move, the facing and the cursor point sent; the pointer at the aim.
PadSteer padPlaySteer(App& app, float fromX, float fromY);
/// The pad isn't driving the game this frame: its pointer and aim dropped, nothing gathered.
void padPlayIdle(App& app);

/// A press's controls and pad actions, before they act: with a spell waiting for its target
/// (app.armed), Interact or a slot fires it at the aim instead (true: nothing else to do); else a
/// slot's cast (or the trap) gets the cursor on its aim first, sent at once.
bool padPlayControls(App& app, const pad::PadActionSet& actions, const std::vector<std::string>& controls);
/// The pad's own game actions (Interact, TargetNext / TargetPrevious).
void padPlayAction(App& app, pad::PadAction action, bool down);

/// liveSprites, while app.padPlay.collecting: whether a hoverable drawable is one the pad wants
/// (a creature, or something near us a hand may take); then, once its sight test passes, noted.
bool padWants(const App& app, const nox::client::Drawable& d, const TypeArt& a);
void padNoteSeen(App& app, std::uint16_t netCode, const nox::client::Drawable& d, const TypeArt& a);

/// Whether the pad is the device in use in a game (the 2D view): the mouse's pointer and the hover
/// ring under it aren't drawn; the target's ring and A's prompt are.
bool padHidesPointer(const App& app);
/// The ring under the pad's target (hoverRing's while padHidesPointer), and the one under what A
/// takes.
std::optional<View3D::Ring> padTargetRing(App& app);
std::optional<View3D::Ring> padInteractRing(App& app);
/// The panels' snap cursor and A's prompt, where the pointer would be drawn (drawCursor).
void drawPadOverlay(App& app);

} // namespace noxsdl
