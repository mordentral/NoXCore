// The meters' bottle slots (bottle_slots.hpp says what they hold and when).
#include "bottle_slots.hpp"

#include "noxcore/things/thing_db.hpp"

#include <string_view>

namespace noxsdl {

BottleSlots::Types BottleSlots::typesIn(const nox::things::ThingDb& things)
{
    // guiHealthManaInit: by name, once.
    const auto id = [&](std::string_view name) -> std::uint16_t {
        const nox::things::ThingDef* t = things.findThing(name);
        return t ? static_cast<std::uint16_t>(t - things.things.data() + 1) : 0;
    };
    return Types{id("RedPotion"), id("BluePotion"), id("CurePoisonPotion"), id("RedApple"), id("Meat")};
}

BottleSlots::BottleSlots(Types types)
    : types_(types)
{
    // The mana and cure poison slots' types are set when the meters are made; the health slot's
    // starts empty, for the re-pick to fill.
    type_[kMana] = types_.bluePotion;
    type_[kCurePoison] = types_.curePoison;
}

void BottleSlots::pick(const InventoryGrid& grid)
{
    // The first stack's count of a type, row by row; 0 for none.
    const auto first = [&](std::uint16_t typeId) -> int {
        const auto cell = typeId ? grid.firstOf(typeId) : std::nullopt;
        return cell ? static_cast<int>(grid.cell(*cell).items.size()) : 0;
    };
    count_[kCurePoison] = first(types_.curePoison);
    count_[kMana] = first(types_.bluePotion);
    // Health: a red potion while there's one, else meat, else an apple, else nothing.
    for (const std::uint16_t t : {types_.redPotion, types_.meat, types_.redApple}) {
        if (const int n = first(t)) {
            type_[kHealth] = t;
            count_[kHealth] = n;
            return;
        }
    }
    type_[kHealth] = 0;
    count_[kHealth] = 0;
}

std::optional<std::uint16_t> BottleSlots::item(const InventoryGrid& grid, int slot) const
{
    const std::uint16_t t = type(slot);
    if (t == 0) {
        return std::nullopt;
    }
    const auto cell = grid.firstOf(t);
    if (!cell) {
        return std::nullopt;
    }
    return grid.cell(*cell).items.front();
}

} // namespace noxsdl
