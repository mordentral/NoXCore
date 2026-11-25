// (noxcpp) The game controller's snap cursor on the game's panels (the inventory, the spellbook,
// the bars, the shop...): which spot a direction goes to, the inventory tray's scroll at its edges,
// a held direction's repeat, the left stick read as a direction, the face buttons' actions on the
// inventory, and which slot a slot button is. Pure functions (no SDL), so the tests check them as
// they are; pad_panels.cpp is the App's side.
//
// A spot is a box on the screen (output pixels). A direction goes to the nearest spot that way:
// first among those in its beam (overlapping the spot's extent across the way it goes), by the
// distance along the way plus half the offset across it; then among the rest that lie more that
// way than across (within about 63 degrees), by the distance plus twice the offset.
#pragma once

#include "inventory_grid.hpp"
#include "pad/pad_bindings.hpp"
#include "pad/pad_math.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace noxsdl::pad {

enum class NavDir : std::uint8_t { Up, Down, Left, Right };

/// A spot's box (output pixels).
struct NavBox {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    bool operator==(const NavBox&) const = default;
};

/// The spot `dir` goes to from `from` (an index into `spots`; `from` itself, if it's among them,
/// is never picked), nullopt with none that way.
std::optional<std::size_t> navPick(std::span<const NavBox> spots, const NavBox& from, NavDir dir);
/// The spot nearest `to`'s middle (the one a lost selection falls back to), nullopt with none.
std::optional<std::size_t> navNearest(std::span<const NavBox> spots, const NavBox& to);

/// The inventory's tray as it's in view: its first row shown, how many show, how far down the
/// cursor may scroll it (the last row it can reach).
struct TrayView {
    int firstRow = 0;
    int shown = 3;
    int lastRow = 19;
};
/// A step up from the top row in view, or down from the bottom one, while the tray has rows that
/// way: the row stepped to and the scroll (-1 a row up, +1 a row down). Nullopt for a step that
/// stays in view, or past the tray's end (the spatial pick's then).
struct TrayStep {
    int row = 0;
    int scroll = 0;
    bool operator==(const TrayStep&) const = default;
};
std::optional<TrayStep> trayEdgeStep(int row, const TrayView& view, NavDir dir);
/// How far down the cursor may scroll the tray: a row past the last that holds anything (an empty
/// row to move things into), all of it while an item is held, never past `last` (19).
int trayLastRow(int lastFilledRow, bool holding, int last = 19);

/// A held direction's repeat (the D-pad's or the left stick's): a step on its press, then the
/// first repeat after kFirstMs and the next every kEveryMs.
class NavRepeat {
public:
    static constexpr std::uint64_t kFirstMs = 380;
    static constexpr std::uint64_t kEveryMs = 110;

    /// A D-pad press (its step taken at once by the caller): its repeat begins.
    void press(NavDir dir, std::uint64_t nowMs)
    {
        held_ = dir;
        nextMs_ = nowMs + kFirstMs;
    }
    /// Each frame: the way held now (none: let go). The step due: a new way's at once, a held
    /// one's repeats.
    std::optional<NavDir> frame(std::optional<NavDir> heldNow, std::uint64_t nowMs);
    void clear() { held_.reset(); }

private:
    std::optional<NavDir> held_;
    std::uint64_t nextMs_ = 0;
};

/// The left stick read as a direction (the larger of its two leans): taken at kStickPress of
/// tilt, kept till it's under kStickLetGo.
inline constexpr float kStickPress = 0.55f;
inline constexpr float kStickLetGo = 0.35f;
std::optional<NavDir> stickDir(StickVec v, std::optional<NavDir> held);
/// The D-pad's direction for a button, if it's one of its four.
std::optional<NavDir> dpadDir(PadButton b);

/// What a button does on a panel's spot (by its place: South A, East B, West X, North Y, the
/// bumpers). RB is the inventory's secondary weapon and the book's next page, LB the book's page
/// before.
enum class PanelPress : std::uint8_t { None, Accept, Back, Drop, Move, Secondary, PreviousPage };
PanelPress panelPress(PadButton b);

/// The inventory drag's end (inventory_grid.hpp) a pad action stands for on an item: A a quick
/// click (use, wear, take off), X out of the panel (dropped), RB onto the secondary box; Y's place
/// is where the cursor is (the caller's). Nullopt for the others.
std::optional<DragEnd> padDragEnd(PanelPress p);

/// The slot (0-4) a button fires in play: the InvokeSlot1-5 it's bound to alone (not on the
/// modifier's layer), from the config's bindings. Nullopt: none.
std::optional<int> slotOfButton(const PadConfig& config, PadButton b);

} // namespace noxsdl::pad
