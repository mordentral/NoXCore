--!strict
-- The vendor (design 4.4) at the town's anvil: it sells potions (always) and a few pieces of gear
-- rolled at the area level, buys what a player carries for gold by its rarity and level, and for
-- gold rerolls a magic or rare piece's modifiers (and a rare's affixes) at its level.
--
-- Its stock is the game's, not a character's: rolled as each area is entered and again when an
-- act is cleared, and kept nowhere (a new game's vendor rolls its own). Prices are
-- data/vendor.luau's. A sale, a purchase or a reroll is one step on the server, the record written
-- at once (as the stash's moves are: an item is never in two places, and gold never counted twice).

local hero = require("hero")
local items = require("items")
local loot = require("loot")
local windows = require("windows")
local hud = require("hud")
local data = require("data/vendor")
local lootData = require("data/loot")

local vendor = {}

-- The stock now: { { rec | type (a potion), price, potion } }, and the area level it was rolled at.
local stock: { any } = {}
local stockLevel = 1

local potionPrice: { [string]: number } = {}
for _, p in data.potions do
	potionPrice[p.type] = p.price
end

-- A piece's price at the vendor (what it sells it for).
function vendor.price(rec: any): number
	local p = potionPrice[rec.type]
	if p then
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

-- The stock rolled again at the area level.
function vendor.restock(level: number)
	stockLevel = math.max(1, level)
	table.clear(stock)
	for _, p in data.potions do
		table.insert(stock, { type = p.type, price = p.price, potion = true })
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
		local rec = loot.makeGear(pick.type, rarity, stockLevel)
		table.insert(stock, { rec = rec, price = vendor.price(rec) })
	end
	-- (anyone looking at it sees the new stock)
	for _, h in hero.all() do
		if windows.isOpen(h, "vendor") then
			vendor.view(h)
		end
	end
end

-- The stock shown.
function vendor.view(h: any)
	local list = {}
	for i, s in stock do
		local e = if s.potion then windows.entry({ type = s.type, rarity = "normal" }) else windows.entry(s.rec)
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
	local s = stock[math.floor(i)]
	if not s then
		return false, "not for sale"
	end
	local gold = nox.gold(h.unit)
	if gold < s.price then
		return false, "not enough gold"
	end
	local id = if s.potion then items.give(h.unit, { type = s.type, rarity = "normal" }, false) else items.give(h.unit, s.rec, false)
	if not id then
		return false, "it couldn't be given"
	end
	nox.setGold(h.unit, gold - s.price, true)
	if not s.potion then
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
	local fresh = loot.makeGear(rec.type, rec.rarity, rec.level or stockLevel)
	if fresh.rarity ~= rec.rarity then
		return false, "that piece can't be rerolled"
	end
	local id = items.give(h.unit, fresh, false)
	if not id then
		return false, "it couldn't be given"
	end
	items.forget(item)
	nox.delete(item)
	nox.setGold(h.unit, gold - price, true)
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
-- { op = "reroll", item }.
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
	end
	if not ok then
		windows.note(h, "vendor", tostring(why))
	end
end

-- The stock as lines, "index type rarity price" (for "dd vendor").
function vendor.lines(): { string }
	local out = { "level " .. stockLevel }
	for i, s in stock do
		if s.potion then
			table.insert(out, tostring(i) .. " " .. tostring(s.type) .. " potion " .. tostring(s.price))
		else
			table.insert(out, tostring(i) .. " " .. tostring(s.rec.type) .. " " .. tostring(s.rec.rarity) .. " " .. tostring(s.price))
		end
	end
	return out
end

windows.register("vendor", { station = "vendor", view = vendor.view, price = vendor.sellPrice, reroll = vendor.rerollPrice })

return vendor
