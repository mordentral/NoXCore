// The inventory window's grid as guiinv.c keeps it (inventory_grid): 4
// columns of 21 rows, cell (column, row) at column * 21 + row, each a stack of one type (its
// drawable's) with up to 32 net codes in order, the first the one shown and taken. Rows 0-19 are
// the ones filled and saved; a drag can put a thing in row 20.
//
// Its order is what a solo save's Inventory section writes (plr_rwInventory walks
// rows 0-19, each row's columns, each stack in order), so it's kept as the exe keeps it:
//   add      Gui_Inv_AddPickedUpItem: onto the first stack of its type with room, row by
//            row (rows 0-19, under 32, never a NOT_STACKABLE type), at its end;
//            else the first empty cell, row by row.
//   remove   Gui_Inv_OnItemDropped: out of its cell, the rest moving up,
//            then the grid closed up.
//   take     a drag's start (cliInventorySpriteUpd): the cell's first out;
//            it goes back at the end of a stack or, moved, the grid is closed up.
//   compact  each empty cell in rows 0-19, row by row, filled by the next cell
//            that isn't; a stackable one first poured into the other stacks of its type (each
//            row from the top, one a row: its last first, up to 32), what's left moved.
// No SDL: noxsdl's Inventory draws it; the tests run it.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace noxsdl {

class InventoryGrid {
public:
    static constexpr int kCols = 4;
    static constexpr int kRows = 21;
    static constexpr int kFilledRows = 0x14; ///< rows 0-19: placement, stacking and the save's walk
    static constexpr std::size_t kMost = 32;  ///< the most net codes a cell holds
    struct Cell {
        std::uint16_t typeId = 0;          ///< its drawable's type
        std::vector<std::uint16_t> items;  ///< the net codes stacked in it
    };
    /// Whether a type may stack (not NOT_STACKABLE, class 0x4000000).
    using Stackable = std::function<bool(std::uint16_t typeId)>;

    explicit InventoryGrid(Stackable stackable = {})
        : stackable_(std::move(stackable))
    {
    }

    static constexpr int index(int col, int row) { return col * kRows + row; }
    const Cell& cell(int i) const { return cells_[static_cast<std::size_t>(i)]; }
    const Cell& at(int col, int row) const { return cell(index(col, row)); }

    /// The cell holding `netCode` (rows 0-20, row by row).
    std::optional<int> find(std::uint16_t netCode) const;
    /// cliInventoryFirstItemByTT: the first cell holding `typeId` (rows 0-20, row by row).
    std::optional<int> firstOf(std::uint16_t typeId) const;
    /// Gui_Inv_AddPickedUpItem's filing; the cell it went into, nullopt with no room
    /// (InventoryFull).
    std::optional<int> add(std::uint16_t netCode, std::uint16_t typeId);
    /// Gui_Inv_OnItemDropped: out of its cell and the grid closed up. Whether it was there.
    bool remove(std::uint16_t netCode);
    /// A drag's start: the first of a cell taken out. Its net code, nullopt for an
    /// empty cell.
    std::optional<std::uint16_t> take(int cell);
    /// Into a cell (rows 0-20) that's empty, or a stack of its type (not
    /// NOT_STACKABLE) under 32, at its end. Whether it went.
    bool put(int cell, std::uint16_t netCode, std::uint16_t typeId);
    /// The inventory window's drop of a thing taken from cell `from` (the other drop's too, with no
    /// `to`): into cell `to` when it was `from`'s last and put takes it there, the grid then closed
    /// up; else back at the end of `from`'s stack (refused there too: out of the
    /// grid). Whether it moved.
    bool drop(int from, std::optional<int> to, std::uint16_t netCode, std::uint16_t typeId);
    /// The grid closed up.
    void compact();
    /// The net codes in the save's order (rows 0-19, each row's columns, each stack in order).
    std::vector<std::uint16_t> order() const;
    void clear() { cells_ = {}; }

private:
    bool stacks(std::uint16_t typeId) const { return !stackable_ || stackable_(typeId); }
    Cell& mut(int col, int row) { return cells_[static_cast<std::size_t>(index(col, row))]; }

    std::array<Cell, kCols * kRows> cells_{};
    Stackable stackable_;
};

} // namespace noxsdl
