// Items with their modifiers and a player's own facts.
// (Internal to the script modes' sources.)
//
// - giveItem and create take a spec: the four modifiers by modifier.bin's names (effectiveness,
//   material, primary, secondary), durability and charges. It's applied after the item is made,
//   before anyone hears of it (Items::give's `prepare`; create's within the frame), so the pickup
//   and equip messages carry it. A modifier that doesn't fit the item by the rule a quest's items
//   are held to (questModifiersValid) is an error: no item the game never makes.
// - nox.item, nox.modifier, nox.modifiers, nox.modifierFits, nox.equip, nox.unequip.
// - A player's table (nox.player, every hook's) gains account, class, level and experience;
//   nox.setLevel, nox.setExperience, nox.giveExperience. A held-out stock player's unit is refused
//   by each (toolbox::playerUnitArg).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

struct lua_State;

namespace nox::server {
struct HostedPlayer;
struct ServerObject;
} // namespace nox::server

namespace nox::modes::script {

struct ItemSpec {
    std::array<std::uint8_t, 4> mods{0xFF, 0xFF, 0xFF, 0xFF};
    std::optional<int> durability;
    std::optional<int> charges;
    bool any() const { return durability || charges || mods != std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF}; }
};

/// The spec table at `idx` (none or nil: an empty spec); an error for a modifier modifier.bin
/// hasn't, or one of another slot's section.
ItemSpec itemSpecArg(lua_State* L, int idx);
/// The spec's durability and charges on a new item, its modifiers (set already) checked; false:
/// why (the item isn't to be kept).
bool finishItem(lua_State* L, server::ServerObject& item, const ItemSpec& spec, std::string& why);
/// account, class, level and experience into the player table on top of the stack.
void addPlayerFacts(lua_State* L, const server::HostedPlayer& p, const server::ServerObject* unit);
/// The items' and players' toolbox functions into the `nox` table on top of the stack.
void addItemsToolbox(lua_State* L);

} // namespace nox::modes::script
