// Shops and trading: a player at a shopkeeper's, or two traders.
//
//   - Sessions (World::trades, TradeSession): Trade_StartSession (MSG_TRADE 0x15 from a
//     client: a shopkeeper, subclass 8), a shop's (createPlayerShopSession, its
//     stock loadShopItems: the keeper's records, or a quest's own), the stock in
//     its order (addItemToShopSession, by each item's key), the start messages
//     (TRADE 0x0D / 0x0C, the stock's 0x08s), a solo player frozen at a shop.
//   - A shop's business: Shop_OnBuyItem, Shop_OnBuyStack (its 1.2b gold bug
//     kept), Shop_OnSellItem, Shop_OnSellStack, Shop_OnAskSellPrice,
//     Shop_OnAskRepairCost, Shop_OnRepairItem; the prices
//     shopGetItemCost (itemCost).
//   - Trading: offers (Trade_AddOffer, Trade_WithdrawOffer), the totals and the gold put up,
//     accepting (Trade_OnAccept), the end (Trade_CloseP2P).
//   - The ends: Trade_ExitShop (a quest's shop kept for the player), the cancel
//     shopCancelSession (a map change, Plr_Die, Plr_RemoveFromGame),
//     a quest's kept shop dropped.
//   - The builtins IsTrading and SetShopkeeperText.
//   - A solo shop's freeze is monsters.hpp's freezeUnit / unfreezeUnit (lock 0).
#pragma once

#include "noxcore/server/world.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace nox::script {
class Vm;
}

namespace nox::server {

struct Fight;

/// shopGetItemCost: what `item` goes for. `mode` 0 a sale to the shop, 1 a buy
/// (or an offer's worth), 2 a repair; `session` the shop's (null: none, no shopkeeper's
/// multipliers). Its worth (a spell book's spell's price, a guide's creature's worth, its
/// modifiers'; a quest's multipliers), its ammunition or charges, the keeper's buy multiplier,
/// its health, the keeper's sell multiplier (gems aside); at least 1; a repair the coefficient of
/// what it lacks. In the exe's floats (its x87 in chop mode). `modifiers` modifier.bin (its
/// modifiers' WORTH; null: none).
int itemCost(const World& world, const things::ModifierDb* modifiers, int mode, const TradeSession* session, const ServerObject& item);
/// ItemIsDroppable (nonzero: kept): a quest item of the table.
bool questItem(const ServerObject& o);

/// The session `id` (PlayerControl::trade), or null.
TradeSession* tradeSession(World& world, std::uint32_t id);
/// Whether it's in a trade (PlayerControl::trade).
inline bool trading(const ServerObject& unit) { return unit.player && unit.player->trade != 0; }

/// Trade_StartSession: a shop when one of them is a player and the other isn't, else a
/// trade; null when either is busy ("StarterAlreadyTrading", "OtherAlreadyTrading").
TradeSession* startTrade(Fight& fight, ServerObject& starter, ServerObject& other);
/// Trade_ExitShop.
void exitShop(Fight& fight, TradeSession& session);
/// Trade_CloseP2P.
void closeTrade(Fight& fight, TradeSession& session);
/// shopCancelSession on `unit`'s trade, if any, and its trade cleared (its
/// callers' trade = 0).
void cancelTrade(Fight& fight, ServerObject& unit);
/// a quest's shop kept for player index `index` freed.
void dropQuestShop(World& world, std::uint8_t index);
/// The cancels at a map change (mapSwitchLevel's loop over the players: thawed
/// with 1, the trade cancelled), on the old map.
void mapChangeTrades(Fight& fight);

/// SrvNet_OnPacket's MSG_TRADE (0xC9) from `unit`'s client: `m` the whole message.
void onTradeMessage(Fight& fight, ServerObject& unit, std::span<const std::uint8_t> m);

/// IsTrading: whether the host (player slot 0x1F) trades.
void builtinIsTrading(World& world, script::Vm& vm);
/// SetShopkeeperText: pops the text, then the object (`popObject`); a shopkeeper's
/// greeting when it's under 32 letters.
void builtinSetShopkeeperText(World& world, script::Vm& vm, const std::function<ServerObject*()>& popObject);

} // namespace nox::server
