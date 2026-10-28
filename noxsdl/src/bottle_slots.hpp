// The meters' three bottle slots (guimeter.c): what the health, mana and cure poison slots hold,
// how many they show, and which item their keys and clicks use.
//
//   types    the mana slot is always BluePotion and the cure poison slot CurePoisonPotion; the
//            health slot is RedPotion while we carry one, else Meat, else RedApple, else
//            nothing (the meters' re-pick). No other food goes in it.
//   counts   each slot shows how many are in the FIRST stack of its type, row by row
//            (cliInventoryFirstItemByTT; a stack holds 32 at most), not every one we carry.
//   re-pick  the types and counts are worked out again only when a food thing (class FOOD:
//            the potions too) is filed into the grid or goes out of it (Gui_Inv_AddPickedUpItem,
//            Gui_Inv_OnItemDropped), and when the meters are made; dragging a stack about leaves
//            them as they were.
//   use      the slot's key or a click uses the first item of the first stack of the slot's type
//            in the grid then, if there's one (quickHealthPotion, guiBottleSlotProc,
//            cliUseCurePoison); the slot with no type does nothing.
// No SDL: the inventory keeps it, the HUD draws it, the tests run it.
#pragma once

#include "inventory_grid.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace nox::things {
struct ThingDb;
}

namespace noxsdl {

class BottleSlots {
public:
    static constexpr int kHealth = 0, kMana = 1, kCurePoison = 2;
    /// The type ids the slots look for (guiHealthManaInit's, by name; 0 for a type not in thing.bin).
    struct Types {
        std::uint16_t redPotion = 0;
        std::uint16_t bluePotion = 0;
        std::uint16_t curePoison = 0;
        std::uint16_t redApple = 0;
        std::uint16_t meat = 0;
    };
    /// The types named in `things` (type ids are thing.bin's index + 1).
    static Types typesIn(const nox::things::ThingDb& things);

    // (Two constructors, not `Types types = {}`: GCC can't use Types' member initializers in a
    // default argument while BottleSlots is still incomplete.)
    BottleSlots() : BottleSlots(Types{}) {}
    explicit BottleSlots(Types types);

    /// The meters' re-pick: the health slot's type, and every slot's count.
    void pick(const InventoryGrid& grid);
    /// The type slot `slot` holds (0: none).
    std::uint16_t type(int slot) const { return valid(slot) ? type_[static_cast<std::size_t>(slot)] : 0; }
    /// How many the slot shows (0: nothing drawn but its key).
    int count(int slot) const { return valid(slot) ? count_[static_cast<std::size_t>(slot)] : 0; }
    /// The item the slot's key would use now: the first of the first stack of its type, row by
    /// row; nullopt with no type or none of it in the grid.
    std::optional<std::uint16_t> item(const InventoryGrid& grid, int slot) const;

private:
    static bool valid(int slot) { return slot >= 0 && slot < 3; }

    Types types_;
    std::array<std::uint16_t, 3> type_{};
    std::array<int, 3> count_{};
};

} // namespace noxsdl
