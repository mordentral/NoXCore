// (noxcpp) The Controller screen's changes to noxcore_controller.cfg's buttons (no SDL): its rows,
// what each row's two cells hold, a button put in a cell (taken from wherever it was), and the
// "press a button" wait, whose rule lets the menus' Back button be bound too.
//
//   rows      every action, the pad's most used first (attack, interact, the slots, jump, the
//             potions...), then the rest of the game's controls in their table's order, the
//             menus' own last.
//   cells     a game action's: its button alone (0) and on the modifier's layer (1, "LT +"); the
//             Modifier has no second cell (no layer for the modifier itself). A menus' action
//             (MenuAccept, MenuBack, the tabs: no layer) has a first and a second button.
//   a button  put in a cell leaves the cells of the same kind it was in (the game's buttons alone,
//             the game's on the modifier's layer, or the menus'), as Input Config's keys do: those
//             actions lose it. The modifier and the menus' accept and back are never left with
//             none: one that would be takes the cell's old button instead (a swap). A new modifier
//             takes its button off the modifier's layer. Guide is the system's: never bound.
//   the wait  a button binds as it goes down, but the menus' Back buttons (East and Start by
//             default) bind as they come up after a short press: held kCancelMs they cancel the
//             wait instead (Escape on the keyboard cancels it too). A button held when the wait
//             began counts only once pressed again (the A that opened it).
#pragma once

#include "pad/pad_bindings.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::pad {

/// The screen's rows, in order.
std::span<const std::string_view> remapRows();
/// Whether an action is the menus' (MenuAccept, MenuBack, the tabs): buttons of their own, apart from
/// the game's, and no modifier's layer.
bool menuRow(std::string_view action);
/// An action's name wherever the controller's screens show it (the rows, the picture's labels, the
/// notices), in the player's language whatever language nox.csf is in. The game's controls read as
/// the English game names them. Empty for a name that isn't an action.
const std::string& actionTitle(std::string_view action);
/// Whether a cell takes a button (the Modifier's second doesn't).
bool cellUsable(std::string_view action, int cell);
/// Whether a button can be bound (not Guide).
bool bindableButton(PadButton b);

/// What a cell holds: a game action's first binding alone (cell 0) or on the modifier's layer (1);
/// a menus' action's first or second.
std::optional<PadButton> cellButton(const PadConfig& c, std::string_view action, int cell);

struct AssignResult {
    bool done = false;                  ///< false: refused (that cell or that button can't), nothing changed
    std::vector<std::string> movedFrom; ///< the actions that lost the button
    std::vector<std::string> swapped;   ///< those of them given the cell's old button in its place
};
/// `b` put in `action`'s `cell` (its other buttons of that kind replaced), taken from the cells it
/// was in.
AssignResult assignCell(PadConfig& c, std::string_view action, int cell, PadButton b);
/// The cell emptied; false (nothing changed) for the last button of the modifier or the menus'
/// accept or back.
bool clearCell(PadConfig& c, std::string_view action, int cell);

/// What an action's prompt shows: its first binding alone, else its first on the modifier's layer
/// (then with the modifier's button). Nothing when it has neither (or the layer has no modifier).
struct ActionButtons {
    std::optional<PadButton> modifier;
    std::optional<PadButton> button;
};
ActionButtons actionButtons(const PadConfig& c, std::string_view action);

/// The "press a button" wait (the file's header).
class CaptureWait {
public:
    static constexpr std::uint64_t kCancelMs = 800;

    struct Outcome {
        enum Kind : std::uint8_t { Waiting, Bound, Cancelled };
        Kind kind = Waiting;
        PadButton button = PadButton::South;
    };

    /// Waits from `nowMs`; `back` the menus' Back buttons (MenuBack's), a bit each by PadButton.
    void start(std::uint64_t nowMs, std::uint32_t back);
    void stop() { active_ = false; }
    bool active() const { return active_; }

    Outcome press(PadButton b, std::uint64_t nowMs);
    Outcome release(PadButton b, std::uint64_t nowMs);
    /// Each frame: a Back button held long enough cancels.
    Outcome tick(std::uint64_t nowMs);
    /// How near a held Back button is to cancelling, 0 to 1 (0: none held).
    float cancelShare(std::uint64_t nowMs) const;

private:
    static std::uint32_t bit(PadButton b) { return 1u << static_cast<unsigned>(b); }
    bool active_ = false;
    std::uint32_t back_ = 0;
    std::uint32_t pressed_ = 0; ///< pressed since the wait began
    std::optional<PadButton> holding_;
    std::uint64_t holdFrom_ = 0;
};

/// The menus' Back buttons in `c` (MenuBack's), a bit each.
std::uint32_t backButtons(const PadConfig& c);

} // namespace noxsdl::pad
