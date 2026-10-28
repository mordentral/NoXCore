--!strict
-- Loot (design 4.3): what a death drops, its rarity and modifiers, and how it shows on the
-- ground until it's picked up.
--
-- Rarity rules: normal gear has no modifiers; magic one or two of the item's four modifier slots;
-- rare all of the slots the item takes, plus one or two of Deep Delve's own affixes. A modifier is
-- only ever one that fits the slot of that item (nox.modifierFits: the game's own rule), and no
-- dearer than the area level allows (data/loot.luau's worth).

--
-- Every item the mod puts on the ground (gear, gold, potions) is kept in one index while it lies
-- there, with its label: its name in its rarity's colour, which labels.luau sends to the players
-- near it (NoX Core clients show it while Alt is held). A party's deaths roll more loot
-- (scale.luau's loot factor).

local data = require("data/loot")
local items = require("items")
local scale = require("scale")

local loot = {}

-- Each item lying on the ground from the mod: id -> { code (its net code), text, rarity, x, y }.
local ground: { [number]: any } = {}

-- A thing type's name as words ("LeatherBoots": "Leather Boots").
local function spaced(name: string): string
	local s = string.gsub(name, "(%l)(%u)", "%1 %2")
	return s
end

-- An item on the ground indexed with its label ("gold" for a pile of gold).
local function index(id: number, rec: any, rarity: string, x: number, y: number)
	local o = nox.object(id)
	if not o then
		return
	end
	local words = (data.words :: any)[rarity] or ""
	local text = if rarity == "gold" then "Gold" else ((if words ~= "" then words .. " " else "") .. spaced(rec.type))
	ground[id] = { code = o.netCode, text = text, rarity = rarity, x = x, y = y }
end

-- The ground's index (labels.luau reads it; an entry whose item is gone is dropped there).
function loot.ground(): { [number]: any }
	return ground
end

-- A death's loot rolls for the party now: `rolls` times the party's loot factor, a part of one a
-- chance of one more (one player: `rolls`, nothing drawn).
function loot.rollsFor(rolls: number): number
	local f = scale.factor("loot")
	if f == 1 then
		return rolls
	end
	local want = rolls * f
	local n = math.floor(want)
	if nox.random() < want - n then
		n += 1
	end
	return n
end

-- ---- Picking by weight

local function weighted(list: any, total: number): any
	local roll = nox.random() * total
	for _, e in list do
		roll -= e.weight
		if roll < 0 then
			return e
		end
	end
	return list[#list]
end

local function eligible(list: any, level: number): (any, number)
	local out, total = {}, 0
	for _, e in list do
		if (e.from or 1) <= level then
			table.insert(out, e)
			total += e.weight
		end
	end
	return out, total
end

-- ---- The modifiers that may go on each item type, by slot (worked out once a type, as asked)

-- type -> slot -> { { name, worth } } in modifier.bin's order.
local pools: { [string]: any } = {}
local sectionOf: { [string]: string } = { effectiveness = "effectiveness", material = "material", primary = "enchantment", secondary = "enchantment" }

local function poolFor(thingType: string): any
	local pool = pools[thingType]
	if pool then
		return pool
	end
	pool = {}
	for _, slot in items.SLOTS do
		local fits = {}
		for _, name in nox.modifiers(sectionOf[slot]) do
			if nox.modifierFits(thingType, slot, name) then
				local m = nox.modifier(name)
				table.insert(fits, { name = name, worth = if m then m.worth else 0 })
			end
		end
		pool[slot] = fits
	end
	pools[thingType] = pool
	return pool
end

-- A modifier for the slot at the area level: one of those the level's worth allows, else the
-- cheapest that fits (an item takes what it takes); nil when none fits at all.
local function pickModifier(pool: any, slot: string, level: number, taken: { [string]: boolean }): string?
	local cap = math.min(data.worth.base + data.worth.perLevel * level, data.worth.most)
	local choices = {}
	local cheapest = nil
	for _, m in pool[slot] do
		if m.worth > 0 and m.worth <= data.worth.most and not taken[m.name] then
			if m.worth <= cap then
				table.insert(choices, m.name)
			end
			if not cheapest or m.worth < cheapest.worth then
				cheapest = m
			end
		end
	end
	if #choices > 0 then
		return choices[nox.random(#choices)]
	end
	return if cheapest then cheapest.name else nil
end

-- ---- Rolling

-- A rarity for gear at the area level ("normal", "magic", "rare"); `better` (0 or more) moves the
-- odds up, as a tougher monster's drops are.
function loot.rollRarity(level: number, better: number?): string
	local r = data.rarity
	local magic = r.magic + r.perLevel.magic * level
	local rare = r.rare + r.perLevel.rare * level
	local b = better or 0
	magic *= 1 + b
	rare *= 1 + 2 * b
	local roll = nox.random() * (r.normal + magic + rare)
	if roll < rare then
		return "rare"
	elseif roll < rare + magic then
		return "magic"
	end
	return "normal"
end

-- An affix's value at the area level.
local function affixValue(def: any, level: number): number
	local lo = def.low + def.perLevel * (level - 1)
	local hi = def.high + def.perLevel * (level - 1)
	return math.floor(lo + nox.random() * (hi - lo) + 0.5)
end

-- A gear record of a type and rarity at the area level: its modifiers by the rarity's rule.
function loot.makeGear(thingType: string, rarity: string, level: number): any
	local rec: any = { type = thingType, rarity = rarity, level = level }
	if rarity == "normal" then
		return rec
	end
	local pool = poolFor(thingType)
	local open = {}
	for _, slot in items.SLOTS do
		if #pool[slot] > 0 then
			table.insert(open, slot)
		end
	end
	local taken = {}
	local function fill(slot: string)
		local name = pickModifier(pool, slot, level, taken)
		if name then
			rec[slot] = name
			taken[name] = true -- (never the same enchantment twice)
		end
	end
	if rarity == "magic" then
		-- One or two of its slots, drawn.
		local want = math.min(#open, nox.random(2))
		for _ = 1, want do
			local i = nox.random(#open)
			fill(open[i])
			table.remove(open, i)
		end
	else
		-- Every slot it takes, then its affixes.
		for _, slot in open do
			fill(slot)
		end
		local defs = table.clone(data.affixes)
		rec.affixes = {}
		for _ = 1, math.min(#defs, nox.random(2)) do
			local def: any = table.remove(defs, nox.random(#defs))
			table.insert(rec.affixes, { id = def.id, value = affixValue(def, level) })
		end
	end
	-- An item that takes no modifier at all stays normal (its affixes with it).
	if not (rec.effectiveness or rec.material or rec.primary or rec.secondary) then
		rec.rarity = "normal"
		rec.affixes = nil
	end
	return rec
end

-- One loot roll at the area level: { kind = "gold" } | { kind = "potion", type } |
-- { kind = "gear", record } | nil.
function loot.rollOne(level: number, better: number?): any
	local r = data.roll
	local roll = nox.random() * 100
	if roll < r.gold then
		return { kind = "gold" }
	end
	roll -= r.gold
	if roll < r.potion then
		local all, total = eligible(data.potions, level)
		return { kind = "potion", type = weighted(all, total).type }
	end
	roll -= r.potion
	if roll < r.gear * (1 + (better or 0)) then
		local all, total = eligible(data.gear, level)
		local g = weighted(all, total)
		return { kind = "gear", record = loot.makeGear(g.type, loot.rollRarity(level, better), level) }
	end
	return nil
end

-- ---- On the ground

-- Each item lying with sparks: item id -> { op, x, y, every, next (frame) }. One beat for all
-- of them (every quarter second) sends those due; picked up, an item's entry goes.
local shown: { [number]: any } = {}
local count = 0
local beat: number? = nil

local function pulse()
	local now = nox.frame()
	for _, e in shown do
		if now >= e.next then
			nox.effect(e.op, e.x, e.y)
			e.next = now + e.every
		end
	end
end

local function showOnGround(id: number, rarity: string, x: number, y: number)
	local g = (data.ground :: any)[rarity]
	if not g then
		return
	end
	if g.sound then
		nox.soundAt(x, y, g.sound)
	end
	if g.sparks and count < data.groundMost then
		shown[id] = { op = g.sparks, x = x, y = y, every = g.every, next = nox.frame() }
		count += 1
		if not beat then
			beat = nox.every(8, pulse)
		end
	end
end

local function hide(id: number)
	if shown[id] then
		shown[id] = nil
		count -= 1
	end
end

-- A record's item put on the ground at (x, y), shown by its rarity; its id.
function loot.place(rec: any, x: number, y: number): number?
	local ok, id = pcall(nox.create, rec.type, x, y, nil, items.specOf(rec))
	if not ok or not id then
		nox.log("deep_delve: loot not made: " .. tostring(rec.type) .. " (" .. tostring(id) .. ")")
		return nil
	end
	items.setMeta(id, { rarity = rec.rarity or "normal", affixes = rec.affixes, level = rec.level })
	showOnGround(id, rec.rarity or "normal", x, y)
	index(id, rec, rec.rarity or "normal", x, y)
	return id
end

-- A death's loot at (x, y): `rolls` rolls at the area level; the ids made.
function loot.drop(x: number, y: number, level: number, rolls: number, better: number?): { number }
	local made = {}
	for _ = 1, rolls do
		local d = loot.rollOne(level, better)
		if d then
			-- (a little apart, so a pile reads as several things)
			local px, py = nox.clearPointNear(x + nox.random(-20, 20), y + nox.random(-20, 20), 30)
			local id: number? = nil
			if d.kind == "gold" then
				id = nox.create("Gold", px, py)
				if id then
					index(id, { type = "Gold" }, "gold", px, py)
				end
			elseif d.kind == "potion" then
				id = nox.create(d.type, px, py)
				if id then
					index(id, { type = d.type }, "normal", px, py)
				end
			else
				id = loot.place(d.record, px, py)
			end
			if id then
				table.insert(made, id)
			end
		end
	end
	return made
end

-- An act boss's drop at (x, y): d.rare pieces of gear that are rare (or the item's best: one that
-- takes no modifier can't be), d.levelUp levels above the area's, then d.rolls loot rolls with
-- d.better odds; the ids made.
function loot.bossDrop(x: number, y: number, level: number, d: any): { number }
	local made = {}
	local all, total = eligible(data.gear, level + d.levelUp)
	for i = 1, d.rare do
		local rec = nil
		for _ = 1, 8 do
			rec = loot.makeGear(weighted(all, total).type, "rare", level + d.levelUp)
			if rec.rarity == "rare" then
				break
			end
		end
		local a = i * 2.4
		local px, py = nox.clearPointNear(x + 30 * math.cos(a), y + 30 * math.sin(a), 30)
		local id = loot.place(rec, px, py)
		if id then
			table.insert(made, id)
		end
	end
	for _, id in loot.drop(x, y, level, loot.rollsFor(d.rolls), d.better) do
		table.insert(made, id)
	end
	return made
end

-- An item picked up (by anyone): its sparks stop, its label goes.
function loot.pickedUp(item: number)
	hide(item)
	ground[item] = nil
end

-- The items sparking on the ground now, as "id effect" lines (for "dd ground").
function loot.sparking(): { string }
	local out = {}
	for id, e in shown do
		table.insert(out, id .. " " .. e.op)
	end
	table.sort(out)
	return out
end

-- A new map: the old one's items went with it.
function loot.reset()
	table.clear(shown)
	table.clear(ground)
	count = 0
end

return loot
