// (noxcpp) What the game controller's snap cursor on the game's panels keeps from frame to frame
// (App::padPanels). The functions that use it are pad_panels.hpp's.
#pragma once

#include "pad/pad_panel_nav.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace noxsdl::pad {

/// The panel a spot is on.
enum class Panel : std::uint8_t { Inventory, Book, Bars, Traps, Summon, MapVote, Mode };

/// A spot of the snap cursor: its panel, its kind there and index (the panel's own numbers: the
/// inventory's and the book's PadSpotKind, a bar slot's set and slot...), its box this frame.
struct Spot {
    Panel panel = Panel::Inventory;
    std::uint8_t kind = 0;
    int index = 0;
    NavBox box;
    bool escapes = false; ///< (a mode's window) Escape puts it away: B sends one

    bool sameAs(const Spot& o) const { return panel == o.panel && kind == o.kind && index == o.index; }
};

struct PanelState {
    /// The snap cursor has the pad this frame (a panel or a window to pick on is up, the pad is in
    /// use, nothing over the game); `still`: it has the left stick too and the player stands (the
    /// inventory, the book, a shop's or a trade's screens).
    bool active = false;
    bool still = false;
    /// The shop's, the dialog's or the trade's windows have the pad's directions (their buttons,
    /// the stock's and the offers' cells: the window system's navigation), not the snap cursor.
    bool inWindows = false;
    std::vector<Spot> spots; ///< this frame's
    std::optional<Spot> at;  ///< the one selected
    NavRepeat repeat;
    std::optional<NavDir> stick; ///< the left stick's way held
    /// Y's move under way: an inventory item on the cursor (the inventory's drag), or a bar's
    /// spell (App::barDrag).
    enum class Holding : std::uint8_t { None, Item, Bar };
    Holding holding = Holding::None;

    void clear()
    {
        active = false;
        still = false;
        spots.clear();
        repeat.clear();
        stick.reset();
    }
};

} // namespace noxsdl::pad
