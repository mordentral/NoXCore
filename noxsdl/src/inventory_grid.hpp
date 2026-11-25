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

/// Where a drag from a cell ends (Inventory::mouseUp).
enum class DragEnd : std::uint8_t {
    Outside,      ///< out of the panel: dropped on the ground
    QuickClick,   ///< a quick click where it was taken: used, worn or taken off
    Doll,         ///< onto the doll: worn
    SecondaryBox, ///< onto the secondary weapon's box
    Cell,         ///< into a cell: moved within the inventory
    Panel,        ///< anywhere else on the panel: back in its cell
};

/// Whether the secondary weapon, taken from its cell, stops being the secondary where its drag
/// ends: dropped out, used or worn. (noxcpp: moved within the inventory, or put back, it stays
/// one. The exe made it none as the drag began, whatever came of it: OB-156, fixed.)
constexpr bool endsSecondary(DragEnd end)
{
    return end == DragEnd::Outside || end == DragEnd::QuickClick || end == DragEnd::Doll;
}

/// What the dragged item is, for what its drag's end asks of the server.
struct DragFacts {
    bool fromCell = true;    ///< taken from a cell (else worn, from the doll)
    bool wearable = false;   ///< a weapon, armour or wand
    bool weaponLike = false; ///< a weapon or wand
    bool equipped = false;
    bool quiver = false;     ///< a quiver (weapon class bit 2)
    bool bowWorn = false;    ///< a bow or crossbow is worn
    /// Dropped from the doll on a cell holding one to wear in its place (a weapon or wand for a
    /// weapon or wand, armour of its kind).
    bool cellSwaps = false;
};
/// (noxcpp) The game controller never stops on the doll (a worn piece is in the tray too): a pad
/// action ending at `end` on a worn item in its cell stands for the drag from the doll where only
/// that one does something else, RB's onto the secondary box (the worn weapon put away, where a
/// cell's own drag there asks nothing). The rest end as the cell's (A takes it off, X drops it).
constexpr bool padActsFromDoll(DragEnd end, bool equipped)
{
    return equipped && end == DragEnd::SecondaryBox;
}
/// What a drag's end asks (Inventory::Actions): Use, Equip, Unequip, Drop (on the ground), the
/// Secondary weapon, PutAwayWeapon (the worn weapon put away on the secondary box), EquipThere
/// (the cell's item worn in the dragged one's place), Move (within the grid: nothing asked), None.
enum class ItemAsk : std::uint8_t { None, Use, Equip, Unequip, Drop, Secondary, PutAwayWeapon, EquipThere, Move };
/// The mouse's drag (Inventory::mouseUp) and the pad's direct actions both end here.
constexpr ItemAsk dragAsk(DragEnd end, const DragFacts& f)
{
    switch (end) {
    case DragEnd::Outside:
        return ItemAsk::Drop;
    case DragEnd::QuickClick:
        return !f.wearable ? ItemAsk::Use : f.equipped ? ItemAsk::Unequip : ItemAsk::Equip;
    case DragEnd::SecondaryBox:
        if (f.fromCell) {
            // A weapon or wand from the tray (not a worn one); a quiver only with a bow or crossbow worn.
            return f.weaponLike && !f.equipped && (!f.quiver || f.bowWorn) ? ItemAsk::Secondary : ItemAsk::None;
        }
        return f.weaponLike ? ItemAsk::PutAwayWeapon : ItemAsk::None;
    case DragEnd::Doll:
        return f.fromCell && f.wearable && !f.equipped ? ItemAsk::Equip : ItemAsk::None;
    case DragEnd::Cell:
        if (!f.fromCell) {
            return f.cellSwaps ? ItemAsk::EquipThere : ItemAsk::Unequip;
        }
        return ItemAsk::Move;
    case DragEnd::Panel:
        return ItemAsk::None;
    }
    return ItemAsk::None;
}

} // namespace noxsdl
