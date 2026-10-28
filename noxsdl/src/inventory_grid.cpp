// The inventory's cell bookkeeping as the exe keeps it (inventory_grid.hpp describes each operation).
#include "inventory_grid.hpp"

#include <algorithm>

namespace noxsdl {

std::optional<int> InventoryGrid::find(std::uint16_t netCode) const
{
    // Row by row, rows 0-20 (where the exe's loop ends), each stack from its first.
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const auto& items = at(col, row).items;
            if (std::ranges::find(items, netCode) != items.end()) {
                return index(col, row);
            }
        }
    }
    return std::nullopt;
}

std::optional<int> InventoryGrid::firstOf(std::uint16_t typeId) const
{
    // Row by row, rows 0-20, a filled cell of the type.
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const Cell& c = at(col, row);
            if (!c.items.empty() && c.typeId == typeId) {
                return index(col, row);
            }
        }
    }
    return std::nullopt;
}

std::optional<int> InventoryGrid::add(std::uint16_t netCode, std::uint16_t typeId)
{
    // The first stack of its type with room (under 32), row by row in rows 0-19;
    // never for a NOT_STACKABLE type. At the stack's end.
    if (stacks(typeId)) {
        for (int row = 0; row < kFilledRows; ++row) {
            for (int col = 0; col < kCols; ++col) {
                Cell& c = mut(col, row);
                if (!c.items.empty() && c.typeId == typeId && c.items.size() < kMost) {
                    c.items.push_back(netCode);
                    return index(col, row);
                }
            }
        }
    }
    // else the first empty cell, row by row in rows 0-19.
    for (int row = 0; row < kFilledRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            Cell& c = mut(col, row);
            if (c.items.empty()) {
                c.typeId = typeId;
                c.items = {netCode};
                return index(col, row);
            }
        }
    }
    return std::nullopt; // (InventoryFull)
}

bool InventoryGrid::remove(std::uint16_t netCode)
{
    // Gui_Inv_OnItemDropped: out of its cell (the ones after it move up; the cell empty, its
    // drawable gone), then the close-up.
    const auto where = find(netCode);
    if (!where) {
        return false;
    }
    Cell& c = cells_[static_cast<std::size_t>(*where)];
    c.items.erase(std::ranges::find(c.items, netCode));
    if (c.items.empty()) {
        c.typeId = 0;
    }
    compact();
    return true;
}

std::optional<std::uint16_t> InventoryGrid::take(int cell)
{
    // cliInventorySpriteUpd: the dragged one is the cell's first (+4), out of it
    // by the cell's removal (index 0). No closing up.
    Cell& c = cells_[static_cast<std::size_t>(cell)];
    if (c.items.empty()) {
        return std::nullopt;
    }
    const std::uint16_t netCode = c.items.front();
    c.items.erase(c.items.begin());
    if (c.items.empty()) {
        c.typeId = 0;
    }
    return netCode;
}

bool InventoryGrid::put(int cell, std::uint16_t netCode, std::uint16_t typeId)
{
    // Into a cell (columns 0-3, rows 0-20): a stack takes only its own type, not a
    // NOT_STACKABLE one, while under 32.
    if (cell < 0 || cell >= kCols * kRows) {
        return false;
    }
    Cell& c = cells_[static_cast<std::size_t>(cell)];
    if (!c.items.empty() && (!stacks(typeId) || c.typeId != typeId)) {
        return false;
    }
    if (kMost <= c.items.size()) {
        return false;
    }
    if (c.items.empty()) {
        c.typeId = typeId;
    }
    c.items.push_back(netCode);
    return true;
}

bool InventoryGrid::drop(int from, std::optional<int> to, std::uint16_t netCode, std::uint16_t typeId)
{
    // (The inventory window's case 7: the click cell's count 0 and a put at the target, then the
    // alt flag's move (never set by then: the cell's removal cleared it) and the close-up; else
    // a put at the click cell.)
    if (to && cell(from).items.empty() && put(*to, netCode, typeId)) {
        compact();
        return true;
    }
    put(from, netCode, typeId);
    return false;
}

void InventoryGrid::compact()
{
    // The close-up: each empty cell of rows 0-19, row by row.
    for (int row = 0; row < kFilledRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            if (!at(col, row).items.empty()) {
                continue;
            }
            for (;;) {
                // The next cell that isn't empty, from this one on, row by row (rows 0-19).
                int fromCol = -1;
                int fromRow = -1;
                for (int r = row; r < kFilledRows && fromRow < 0; ++r) {
                    for (int c = r == row ? col : 0; c < kCols; ++c) {
                        if (!at(c, r).items.empty()) {
                            fromCol = c;
                            fromRow = r;
                            break;
                        }
                    }
                }
                if (fromRow < 0) {
                    return; // (the rest's equipped and secondary marks cleared: none kept here)
                }
                Cell& from = mut(fromCol, fromRow);
                if (stacks(from.typeId)) {
                    // Poured into the other stacks of its type (not full: count 0x20): each row from
                    // the top, the first such stack in it, its last net code first, up to 32.
                    for (int r = 0; r < kFilledRows && !from.items.empty(); ++r) {
                        for (int c = 0; c < kCols; ++c) {
                            Cell& to = mut(c, r);
                            if (to.items.empty() || to.items.size() == kMost || to.typeId != from.typeId || (c == fromCol && r == fromRow)) {
                                continue;
                            }
                            while (!from.items.empty() && to.items.size() < kMost) {
                                to.items.push_back(from.items.back());
                                from.items.pop_back();
                            }
                            break; // (the next row)
                        }
                    }
                    if (from.items.empty()) {
                        from.typeId = 0;
                        continue; // (this empty cell again)
                    }
                }
                // What's left of it moved here (the whole cell, 0x94 bytes).
                Cell& to = mut(col, row);
                to = std::move(from);
                from = Cell{};
                break;
            }
        }
    }
}

std::vector<std::uint16_t> InventoryGrid::order() const
{
    std::vector<std::uint16_t> out;
    for (int row = 0; row < kFilledRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const auto& items = at(col, row).items;
            out.insert(out.end(), items.begin(), items.end());
        }
    }
    return out;
}

} // namespace noxsdl
