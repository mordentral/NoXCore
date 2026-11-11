--!strict
-- Loot (design 4.3): what a death drops, its rarity and modifiers, and how it shows on the
-- ground until it's picked up.
--
-- Rarity rules: normal gear has no modifiers; magic one or two of the item's four modifier slots;
-- rare all of the slots the item takes, plus one or two of Deep Delve's own affixes (only those its
-- kind may carry: affixes.luau). A modifier is
-- only ever one that fits the slot of that item (nox.modifierFits: the game's own rule), does
-- something on it (loot.doesOn: an effect the engine runs, and a number of the item's for it to
-- scale: no armour quality on a shield) and is no dearer than the area level allows
-- (data/loot.luau's worth). Rarely a loot roll's gear is a unique or a set piece instead, and a
-- champion and an act boss roll for one of each besides (uniques.luau, data/uniques.luau).

--
-- Every item the mod puts on the ground (gear, gold, potions) is kept in one index while it lies
-- there, with its label: its name in its rarity's colour, which labels.luau sends to the players
-- near it (NoX Core clients show it while Alt is held). A party's deaths roll more loot
-- (scale.luau's loot factor).

local data = require("data/loot")
local affixes = require("affixes")
local items = require("items")
local scale = require("scale")
local uniques = require("uniques")

local loot = {}

-- Each item lying on the ground from the mod: id -> { code (its net code), text, rarity, kind
-- ("gear", "potion", "gold"), x, y }; and by its net code: code -> id (an entry checked against
-- `ground` as it's read: one dropped there is stale).
local ground: { [number]: any } = {}
local byCode: { [number]: number } = {}

-- A thing type's name as words ("LeatherBoots": "Leather Boots").
local function spaced(name: string): string
	local s = string.gsub(name, "(%l)(%u)", "%1 %2")
	return s
end

-- An item on the ground indexed with its label ("gold" for a pile of gold).
local function index(id: number, rec: any, rarity: string, x: number, y: number, kind: string)
	local o = nox.object(id)
	if not o then
		return
	end
	local words = (data.words :: any)[rarity] or ""
	-- (a unique or a set piece by its own name)
	local text = if rarity == "gold" then "Gold" else (uniques.nameOf(rec) or ((if words ~= "" then words .. " " else "") .. spaced(rec.type)))
	ground[id] = { code = o.netCode, text = text, rarity = rarity, kind = kind, x = x, y = y }
	byCode[o.netCode] = id
end

-- Another thing the mod put on the ground (a locked room's key) labelled as loot is: its type's
-- name as words, `rarity`'s colour, `kind` its own word.
function loot.mark(id: number, rarity: string, kind: string)
	local o = nox.object(id)
	if o then
		index(id, { type = o.type }, rarity, o.x, o.y, kind)
	end
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

-- Whether a modifier does anything: one of its effects is one the engine runs (data/loot's
-- effects). Read once a modifier.
local doesCache: { [string]: boolean } = {}

function loot.does(name: string): boolean
	local known = doesCache[name]
	if known ~= nil then
		return known
	end
	local m = nox.modifier(name)
	local yes = false
	for fn in (if m then m.effects else {}) :: { [string]: number } do
		if (data.effects :: any)[fn] then
			yes = true
			break
		end
	end
	doesCache[name] = yes
	return yes
end

-- Whether a modifier does anything on an item type: as loot.does, but an effect that only scales
-- one of the type's own numbers (data/loot's `scales`) counts only when the type has some of it.
-- The game's shields have no armour value, so an armour quality on one does nothing. Read once a
-- type and modifier.
local doesOnCache: { [string]: boolean } = {}

function loot.doesOn(thingType: string, name: string): boolean
	local key = thingType .. "|" .. name
	local known = doesOnCache[key]
	if known ~= nil then
		return known
	end
	local yes = false
	if loot.does(name) then
		local m = nox.modifier(name)
		local st = nox.itemStats(thingType)
		for fn in (if m then m.effects else {}) :: { [string]: number } do
			if (data.effects :: any)[fn] then
				local stat = (data.scales :: any)[fn]
				if not stat or (st and type((st :: any)[stat]) == "number" and (st :: any)[stat] > 0) then
					yes = true
					break
				end
			end
		end
	end
	doesOnCache[key] = yes
	return yes
end

local function poolFor(thingType: string): any
	local pool = pools[thingType]
	if pool then
		return pool
	end
	pool = {}
	for _, slot in items.SLOTS do
		local fits = {}
		for _, name in nox.modifiers(sectionOf[slot]) do
			-- (only one that fits the slot, as the game's rule has it, and does something)
			if nox.modifierFits(thingType, slot, name) and loot.doesOn(thingType, name) then
				local m = nox.modifier(name)
				table.insert(fits, { name = name, worth = if m then m.worth else 0 })
			end
		end
		pool[slot] = fits
	end
	pools[thingType] = pool
	return pool
end

-- Each modifier of a worth that does nothing (loot.does), as "dd modifiers" lists them: "NAME
-- worth W fits F drops N" (F: the gear types it fits a slot of by the game's rule; N: those it may
-- drop on, doing something there (loot.doesOn) and worth allowing: 0 for every one listed),
-- sorted; with `all`, every modifier of the three sections, "does" or "nothing" after its line.
function loot.modifierLines(all: boolean?): { string }
	local out = {}
	for _, section in { "effectiveness", "material", "enchantment" } do
		for _, name in nox.modifiers(section) do
			local m = nox.modifier(name)
			local worth = if m then m.worth else 0
			local does = loot.does(name)
			if all or (not does and worth > 0) then
				local fits, drops = 0, 0
				for _, g in data.gear do
					local fit = false
					for _, slot in items.SLOTS do
						if sectionOf[slot] == section and nox.modifierFits(g.type, slot, name) then
							fit = true
						end
					end
					if fit then
						fits += 1
						if loot.doesOn(g.type, name) and worth > 0 and worth <= data.worth.most then
							drops += 1
						end
					end
				end
				table.insert(out, string.format("%s worth %d fits %d drops %d", name, worth, fits, drops) .. (if all then (if does then " does" else " nothing") else ""))
			end
		end
	end
	table.sort(out)
	return out
end

-- What fits each slot of a type by the game's rule, as "dd modifiers TYPE" lists it: "SLOT NAME
-- worth W does|nothing" (on that type: loot.doesOn), in modifier.bin's order.
function loot.fitLines(thingType: string): { string }
	local out = {}
	for _, slot in items.SLOTS do
		for _, name in nox.modifiers(sectionOf[slot]) do
			if nox.modifierFits(thingType, slot, name) then
				local m = nox.modifier(name)
				table.insert(out, string.format("%s %s worth %d %s", slot, name, if m then m.worth else 0,
					if loot.doesOn(thingType, name) then "does" else "nothing"))
			end
		end
	end
	return out
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

-- `n` of Deep Delve's own affixes for an item type, each a different one of those its kind may
-- carry (affixes.allowed: a weapon's, armour's, or any piece's; for `class`, only those it may
-- have), at the area level.
local function rollAffixes(thingType: string, n: number, level: number, class: string?): { any }
	local defs = table.clone(affixes.allowed(thingType, class))
	local out = {}
	for _ = 1, math.min(#defs, n) do
		local def: any = table.remove(defs, nox.random(#defs))
		table.insert(out, { id = def.id, value = affixValue(def, level) })
	end
	return out
end

-- A gear record of a type and rarity at the area level: its modifiers by the rarity's rule. An
-- item no modifier of the game's fits at all (the staffs) carries its rarity in Deep
-- Delve's own affixes alone (by decision 2026-11-01): one for magic, two for a rare. `class`: rolled
-- for that class (a vendor's stock, a reroll), its affixes only those it may have; nil: for anyone
-- (a drop on the ground).
function loot.makeGear(thingType: string, rarity: string, level: number, class: string?): any
	local rec: any = { type = thingType, rarity = rarity, level = level }
	if rarity == "normal" then
		return rec
	end
	local pool = poolFor(thingType)
	local open = {}
	for _, slot in items.SLOTS do
		-- (a slot with a modifier that may drop: one of a worth, and not past the most)
		for _, m in pool[slot] do
			if m.worth > 0 and m.worth <= data.worth.most then
				table.insert(open, slot)
				break
			end
		end
	end
	if #open == 0 then
		rec.affixes = rollAffixes(thingType, if rarity == "rare" then 2 else 1, level, class)
		return rec
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
		rec.affixes = rollAffixes(thingType, nox.random(2), level, class)
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
		-- (rarely a unique or a set piece in its place: uniques.luau)
		local special = uniques.rollGear(level, better)
		if special then
			return { kind = "gear", record = special }
		end
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
	items.setMeta(id, items.metaOf(rec))
	showOnGround(id, rec.rarity or "normal", x, y)
	index(id, rec, rec.rarity or "normal", x, y, "gear")
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
					index(id, { type = "Gold" }, "gold", px, py, "gold")
				end
			elseif d.kind == "potion" then
				id = nox.create(d.type, px, py)
				if id then
					index(id, { type = d.type }, "normal", px, py, "potion")
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

-- A champion's or an act boss's own rolls for a unique and a set piece (uniques.rollSpecial;
-- `tier` "champion" or "boss", `boss` the boss's id: its favoured uniques), put round (x, y); the
-- ids made.
function loot.dropSpecial(x: number, y: number, level: number, tier: string, boss: string?): { number }
	local made = {}
	for i, rec in uniques.rollSpecial(level, tier, boss) do
		local a = i * 2.1 + 1
		local px, py = nox.clearPointNear(x + 26 * math.cos(a), y + 26 * math.sin(a), 30)
		local id = loot.place(rec, px, py)
		if id then
			table.insert(made, id)
		end
	end
	return made
end

-- An act boss's drop at (x, y): d.rare pieces of gear that are rare (or the item's best: one that
-- takes no modifier can't be), d.levelUp levels above the area's, then d.rolls loot rolls with
-- d.better odds, then its own rolls for a unique and a set piece at the rare pieces' level (`boss`
-- its id: its favoured uniques); the ids made.
-- A piece of gear that is rare at `level` (or the item's best: one that takes no modifier can't
-- be; up to eight draws), its record.
function loot.rarePiece(level: number): any
	local all, total = eligible(data.gear, level)
	local rec = nil
	for _ = 1, 8 do
		rec = loot.makeGear(weighted(all, total).type, "rare", level)
		if rec.rarity == "rare" then
			break
		end
	end
	return rec
end

function loot.bossDrop(x: number, y: number, level: number, d: any, boss: string?): { number }
	local made = {}
	for i = 1, d.rare do
		local rec = loot.rarePiece(level + d.levelUp)
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
	for _, id in loot.dropSpecial(x, y, level + d.levelUp, "boss", boss) do
		table.insert(made, id)
	end
	return made
end

-- An item picked up (by anyone): its sparks stop, its label goes; whether it was ground loot.
function loot.pickedUp(item: number): boolean
	hide(item)
	local g = ground[item]
	ground[item] = nil
	if g and byCode[g.code] == item then
		byCode[g.code] = nil
	end
	return g ~= nil
end

-- The ground loot with net code `code` still lying there: its id and entry, or nil.
function loot.byCode(code: number): (number?, any)
	local id = byCode[code]
	if not id then
		return nil, nil
	end
	local g = ground[id]
	if not g or g.code ~= code or not nox.present(id) then
		byCode[code] = nil
		return nil, nil
	end
	return id, g
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
	table.clear(byCode)
	count = 0
end

return loot
