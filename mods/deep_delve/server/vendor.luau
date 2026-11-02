--!strict
-- The vendor (design 4.4) at the town's anvil: it sells potions and quivers (always) and a few
-- pieces of gear rolled at the area level, buys what a player carries for gold by its rarity and
-- level, for gold rerolls a magic or rare piece's modifiers (and a rare's affixes) at its level,
-- and repairs what a player carries or wears at the game's own price times a data factor.
--
-- Its stock is each player's own (by decision, for a party): each player sees and buys from a
-- stock of their own. A stock is rolled anew as each area is entered and again when an act is
-- cleared (a "restock"); kept on the hero, never in the record (a new game's vendor rolls its own).
-- At a restock one stock is rolled at once, as it always was, and goes to the first player to
-- look; anyone else's is rolled when they first look (so a player alone sees what the shared stock
-- was). Prices are data/vendor.luau's. A sale, a purchase or a reroll is one step on the server,
-- the record written at once (as the stash's moves are: an item is never in two places, and gold
-- never counted twice).

local affixes = require("affixes")
local hero = require("hero")
local items = require("items")
local loot = require("loot")
local windows = require("windows")
local hud = require("hud")
local data = require("data/vendor")
local lootData = require("data/loot")

local vendor = {}

-- The restock now: its serial (a hero's stock of an older one is rolled again when looked at),
-- its area level, and the stock rolled with it until someone takes it.
local restocks = 0
local stockLevel = 1
local fresh: { any }? = nil

-- The potions' and supplies' own prices, by type (a plain one's: a magic or rare quiver is gear).
local potionPrice: { [string]: number } = {}
for _, p in data.potions do
	potionPrice[p.type] = p.price
end
for _, p in data.supplies do
	potionPrice[p.type] = p.price
end

-- A piece's price at the vendor (what it sells it for).
function vendor.price(rec: any): number
	local p = potionPrice[rec.type]
	if p and (rec.rarity or "normal") == "normal" then
		return p
	end
	local pr = data.prices
	local level = math.max(1, rec.level or 1)
	local factor = (pr.rarity :: any)[rec.rarity or "normal"] or 1
	return math.floor((pr.base + pr.perLevel * level) * factor)
end

-- What the vendor pays for a piece.
function vendor.sellPrice(rec: any): number
	return math.max(1, math.floor(vendor.price(rec) * data.prices.sell))
end

-- What a reroll of a piece costs.
function vendor.rerollPrice(rec: any): number
	return math.max(1, math.floor(vendor.price(rec) * data.prices.reroll))
end

-- One stock at the area level now: { { rec | type (a potion or a supply), price, potion | supply } }:
-- the potions and the supplies (always there, never sold out), then the gear rolled (any of the
-- loot's gear, quivers too, each normal, magic or rare by data/vendor.luau's odds: a magic or rare
-- quiver carries the modifiers that fit a quiver, as a dropped one does). `class`: rolled for a
-- hero of that class, its affixes only those the class may have (none: for anyone).
local function rollStock(class: string?): { any }
	local stock = {}
	for _, p in data.potions do
		table.insert(stock, { type = p.type, price = p.price, potion = true })
	end
	for _, p in data.supplies do
		table.insert(stock, { type = p.type, price = p.price, supply = true })
	end
	local all: { any } = {}
	local total = 0
	for _, g in lootData.gear do
		if (g.from or 1) <= stockLevel then
			table.insert(all, g)
			total += g.weight
		end
	end
	for _ = 1, data.gear.count do
		local roll = nox.random() * total
		local pick = all[#all]
		for _, g in all do
			roll -= g.weight
			if roll < 0 then
				pick = g
				break
			end
		end
		local r = nox.random()
		local rarity = if r < data.gear.rare then "rare" elseif r < data.gear.rare + data.gear.magic then "magic" else "normal"
		local rec = loot.makeGear(pick.type, rarity, stockLevel, class)
		table.insert(stock, { rec = rec, price = vendor.price(rec) })
	end
	return stock
end

-- Whether a stock rolled for anyone suits a class: none of its pieces has an affix the class may
-- not have (mana a second for a warrior: by decision 2026-11-02).
local function suits(list: { any }, class: string): boolean
	for _, s in list do
		for _, a in (s.rec and s.rec.affixes) or {} do
			local def = affixes.def(a.id)
			if def and not affixes.forClass(def, class) then
				return false
			end
		end
	end
	return true
end

-- A hero's own stock: kept while it's of the restock now; else the restock's own stock when nobody
-- has taken it yet and it suits the hero's class, else one rolled for it (for its class) now.
function vendor.stockOf(h: any): { any }
	local mine = h.stock
	if mine and mine.restock == restocks then
		return mine.list
	end
	local class = h.record.identity.class
	local list: { any }
	if fresh and suits(fresh, class) then
		list = fresh
		fresh = nil
	else
		list = rollStock(class)
	end
	h.stock = { restock = restocks, list = list }
	return list
end

-- A restock at the area level: one stock rolled now (as it always was), every hero's own to be
-- rolled again as they look.
function vendor.restock(level: number)
	stockLevel = math.max(1, level)
	restocks += 1
	fresh = rollStock()
	-- (anyone looking at the vendor sees their new stock)
	for _, h in hero.all() do
		if windows.isOpen(h, "vendor") then
			vendor.view(h)
		end
	end
end

-- A piece put at the end of a hero's stock at its price (for testing: "dd vendor stock"); its
-- index.
function vendor.addStock(h: any, rec: any): number
	local stock = vendor.stockOf(h)
	table.insert(stock, { rec = rec, price = vendor.price(rec) })
	if windows.isOpen(h, "vendor") then
		vendor.view(h)
	end
	return #stock
end

-- The stock shown.
function vendor.view(h: any)
	local stock = vendor.stockOf(h)
	local list = {}
	for i, s in stock do
		local e = if s.potion or s.supply then windows.entry(h, { type = s.type, rarity = "normal" }) else windows.entry(h, s.rec)
		e.i = i
		e.p = s.price
		table.insert(list, e)
	end
	hud.parts(h.peer, "stock", windows.serial(), list, { level = stockLevel })
end

local function ready(h: any): (boolean, string?)
	if not h or not h.ready or not nox.present(h.unit) then
		return false, "not now"
	end
	if not windows.near(h, "vendor") then
		return false, "too far from the vendor"
	end
	return true, nil
end

local function after(h: any, why: string)
	hero.save(h, why)
	vendor.view(h)
	windows.sendCarried(h)
end

-- Stock entry `i` bought; whether it was, else why.
function vendor.buy(h: any, i: number): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local stock = vendor.stockOf(h)
	local s = stock[math.floor(i)]
	if not s then
		return false, "not for sale"
	end
	local gold = nox.gold(h.unit)
	if gold < s.price then
		return false, "not enough gold"
	end
	-- (one its class may not use by the pickup, as any class may pick anything up; refused, no gold
	-- is taken and the piece stays for sale)
	local always = s.potion or s.supply
	local id, why = items.hand(h.unit, if always then { type = s.type, rarity = "normal" } else s.rec)
	if not id then
		return false, why
	end
	nox.setGold(h.unit, gold - s.price, true)
	if not always then
		table.remove(stock, math.floor(i))
	end
	after(h, "vendor")
	return true, nil
end

-- A carried item sold; whether it was, else why.
function vendor.sell(h: any, item: number): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local f = nox.item(item)
	local o = nox.object(item)
	if not f or not o or o.pendingDelete or f.holder ~= h.unit then
		return false, "you don't carry that"
	end
	if f.worn then
		return false, "take it off first"
	end
	local rec = items.recordOf(item)
	if not rec then
		return false, "not an item"
	end
	local price = vendor.sellPrice(rec)
	items.forget(item)
	nox.delete(item)
	nox.setGold(h.unit, nox.gold(h.unit) + price, true)
	after(h, "vendor")
	return true, nil
end

-- A carried magic or rare piece's modifiers (and a rare's affixes) rolled again at its level;
-- whether they were, else why.
function vendor.reroll(h: any, item: number): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local f = nox.item(item)
	local o = nox.object(item)
	if not f or not o or o.pendingDelete or f.holder ~= h.unit then
		return false, "you don't carry that"
	end
	if f.worn then
		return false, "take it off first"
	end
	local rec = items.recordOf(item)
	if not rec or not (rec.rarity == "magic" or rec.rarity == "rare") then
		return false, "only magic and rare pieces are rerolled"
	end
	local price = vendor.rerollPrice(rec)
	local gold = nox.gold(h.unit)
	if gold < price then
		return false, "not enough gold"
	end
	-- (rolled for the hero's class: its affixes only those it may have)
	local fresh = loot.makeGear(rec.type, rec.rarity, rec.level or stockLevel, h.record.identity.class)
	if fresh.rarity ~= rec.rarity then
		return false, "that piece can't be rerolled"
	end
	local id, why = items.hand(h.unit, fresh)
	if not id then
		return false, why
	end
	items.forget(item)
	nox.delete(item)
	nox.setGold(h.unit, gold - price, true)
	after(h, "vendor")
	return true, nil
end

-- ---- Repairs (by decision 2026-11-01): the game's own shop repair (nox.repair: durability, and a
-- staff's charges, to their most), at the game's own price (nox.repairCost) times data/vendor.luau's
-- repair factor (by decision 2026-11-02), paid in gold first.

-- A piece's repair price: nil when it needs none.
function vendor.repairCost(item: number): number?
	local cost = nox.repairCost(item)
	if not cost or cost <= 0 then
		return nil
	end
	return math.max(1, math.floor(cost * data.repair.factor + 0.5))
end

-- The hero's pieces that need a repair, worn and carried, in its inventory's order: { { id, rec,
-- facts, cost } }.
function vendor.damaged(h: any): { any }
	local out = {}
	if not nox.present(h.unit) then
		return out
	end
	for _, it in nox.inventory(h.unit) do
		local o = nox.object(it.id)
		local f = nox.item(it.id)
		if o and not o.pendingDelete and f then
			local cost = vendor.repairCost(it.id)
			if cost then
				local rec = items.recordOf(it.id)
				if rec then
					rec.worn = nil
					table.insert(out, { id = it.id, rec = rec, facts = f, cost = cost })
				end
			end
		end
	end
	return out
end

-- A tooltip's lines with its repair price ("pRepair: N gold") after its durability and charges.
function vendor.withRepair(lines: { string }, cost: number): { string }
	local at = #lines + 1
	for i, l in lines do
		local tag = string.sub(l, 1, 1)
		if tag == "y" or tag == "x" then
			at = i
			break
		end
	end
	table.insert(lines, at, "pRepair: " .. tostring(cost) .. " gold")
	return lines
end

-- The pieces needing a repair to the vendor's window ({ i = id, t, r, tl, c = cost } each, worn
-- ones too), and their summed cost in the header; sent with the carried list while the window is
-- open.
function vendor.sendRepairs(h: any)
	local list = {}
	local total = 0
	for _, d in vendor.damaged(h) do
		local e = windows.entry(h, d.rec, d.facts)
		e.tl = vendor.withRepair(e.tl, d.cost)
		e.i = d.id
		e.c = d.cost
		e.w = if d.facts.worn then true else nil
		total += d.cost
		table.insert(list, e)
	end
	hud.parts(h.peer, "repairs", windows.serial(), list, { total = total })
end

-- A piece the hero carries or wears repaired for its price; whether it was, else why.
function vendor.repair(h: any, item: number): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local f = nox.item(item)
	local o = nox.object(item)
	if not f or not o or o.pendingDelete or f.holder ~= h.unit then
		return false, "you don't carry that"
	end
	local cost = vendor.repairCost(item)
	if not cost then
		return false, "it needs no repair"
	end
	local gold = nox.gold(h.unit)
	if gold < cost then
		return false, "not enough gold"
	end
	if not nox.repair(item) then
		return false, "it needs no repair"
	end
	nox.setGold(h.unit, gold - cost, true)
	after(h, "vendor")
	return true, nil
end

-- Every piece the hero carries or wears that needs one repaired, for their summed price (refused
-- whole when the gold is short; only what was repaired is paid); whether any was, else why.
function vendor.repairAll(h: any): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local list = vendor.damaged(h)
	if #list == 0 then
		return false, "nothing needs a repair"
	end
	local total = 0
	for _, d in list do
		total += d.cost
	end
	local gold = nox.gold(h.unit)
	if gold < total then
		return false, "not enough gold (" .. tostring(total) .. ")"
	end
	local paid = 0
	for _, d in list do
		if nox.repair(d.id) then
			paid += d.cost
		end
	end
	nox.setGold(h.unit, gold - paid, true)
	after(h, "vendor")
	return true, nil
end

-- The anvil touched: the vendor's window.
function vendor.touched(unit: number)
	local h = hero.byUnit(unit)
	if h then
		windows.open(h, "vendor")
	end
end

-- A message from the vendor's window: { op = "buy", index } | { op = "sell", item } |
-- { op = "reroll", item } | { op = "repair", item } | { op = "repairAll" }.
function vendor.message(h: any, v: any)
	if not windows.isOpen(h, "vendor") then
		return
	end
	local ok, why = true, nil
	local index, item = tonumber(v.index), tonumber(v.item)
	if v.op == "buy" and index then
		ok, why = vendor.buy(h, index)
	elseif v.op == "sell" and item then
		ok, why = vendor.sell(h, item)
	elseif v.op == "reroll" and item then
		ok, why = vendor.reroll(h, item)
	elseif v.op == "repair" and item then
		ok, why = vendor.repair(h, item)
	elseif v.op == "repairAll" then
		ok, why = vendor.repairAll(h)
	end
	if not ok then
		windows.note(h, "vendor", tostring(why))
	end
end

-- A hero's rolled gear in its stock as lines, "index type rarity effectiveness material primary
-- secondary affixes" (nil for an empty slot; affixes "id:value,..." or "-"), for "dd vendor gear".
function vendor.gearLines(h: any): { string }
	local out = {}
	for i, s in vendor.stockOf(h) do
		local r = s.rec
		if r then
			local list = {}
			for _, a in r.affixes or {} do
				table.insert(list, a.id .. ":" .. tostring(a.value))
			end
			table.insert(out, string.format("%d %s %s %s %s %s %s %s", i, tostring(r.type), tostring(r.rarity), tostring(r.effectiveness), tostring(r.material),
				tostring(r.primary), tostring(r.secondary), if #list > 0 then table.concat(list, ",") else "-"))
		end
	end
	return out
end

-- A hero's stock as lines, "index type rarity price" (for "dd vendor").
function vendor.lines(h: any): { string }
	local out = { "level " .. stockLevel }
	for i, s in vendor.stockOf(h) do
		if s.potion or s.supply then
			table.insert(out, tostring(i) .. " " .. tostring(s.type) .. (if s.potion then " potion " else " supply ") .. tostring(s.price))
		else
			table.insert(out, tostring(i) .. " " .. tostring(s.rec.type) .. " " .. tostring(s.rec.rarity) .. " " .. tostring(s.price))
		end
	end
	return out
end

windows.register("vendor", { station = "vendor", view = vendor.view, price = vendor.sellPrice, reroll = vendor.rerollPrice,
	repairs = vendor.sendRepairs, repairCost = vendor.repairCost, withRepair = vendor.withRepair })

return vendor
