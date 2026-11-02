--!strict
-- Item records and the game's items (design 4.1): an item record is the thing type, its four
-- modifiers by slot (modifier.bin's names), durability and charges, plus Deep Delve's own: its
-- rarity, affixes and level (the area level it was made at, which its price goes by; none on
-- records older than version 3), and a unique's id (`unique`) or a set piece's (`set`):
-- uniques.luau. The game's item carries the first part (A2); the rest is kept
-- here, by the item's id, for as long as the item is in the game (`meta`).

local loot = require("data/loot")
local uniques = require("uniques")
local uniqueData = require("data/uniques")

local items = {}

local SLOTS = { "effectiveness", "material", "primary", "secondary" }
items.SLOTS = SLOTS

-- Each live item's Deep Delve part: id -> { rarity, affixes, level, unique (a unique's id), set (a
-- set piece's id) }.
local meta: { [number]: any } = {}

function items.meta(id: number): any
	return meta[id]
end

-- A record's Deep Delve part, as an item's meta keeps it.
function items.metaOf(rec: any): any
	return { rarity = rec.rarity or "normal", affixes = rec.affixes, level = rec.level, unique = rec.unique, set = rec.set }
end

-- An item's Deep Delve part kept; an item with affixes, a unique and a set piece are watched for
-- being put on and taken off (onEquip: its wearer's numbers and powers change with it).
function items.setMeta(id: number, m: any)
	meta[id] = m
	if (m.affixes and #m.affixes > 0) or m.unique or m.set then
		nox.watchEvents(id, nox.events.equip)
	end
end

-- Items handed over by the pickup (items.hand): their pickup isn't a find to tell of.
local handed: { [number]: boolean } = {}

function items.forget(id: number)
	meta[id] = nil
	handed[id] = nil
end

-- Every id forgotten (a new map: none of the old ones is in it).
function items.reset()
	table.clear(meta)
	table.clear(handed)
end

-- The modifier spec nox.giveItem / nox.create take, from a record.
function items.specOf(rec: any): any
	local spec = {}
	for _, slot in SLOTS do
		spec[slot] = rec[slot]
	end
	spec.durability = rec.durability
	spec.charges = rec.charges
	return spec
end

-- A record of a game item (nil: not an item); worn as the game has it.
function items.recordOf(id: number): any
	local it = nox.item(id)
	if not it then
		return nil
	end
	local rec: any = { type = it.type, worn = it.worn or nil }
	for _, slot in SLOTS do
		rec[slot] = (it :: any)[slot]
	end
	if it.maxDurability > 0 and it.durability < it.maxDurability then
		rec.durability = it.durability
	end
	if it.maxCharges > 0 then
		rec.charges = it.charges
	end
	local m = meta[id]
	rec.rarity = if m then m.rarity else "normal"
	if m and m.affixes and #m.affixes > 0 then
		rec.affixes = m.affixes
	end
	if m and m.level then
		rec.level = m.level
	end
	if m then
		rec.unique, rec.set = m.unique, m.set
	end
	return rec
end

-- A record's item into a player's unit's inventory, put on when `wear` (and the class may);
-- its id, nil when it couldn't be made.
function items.give(unit: number, rec: any, wear: boolean?): number?
	local ok, id = pcall(nox.giveItem, unit, rec.type, false, items.specOf(rec))
	if not ok or not id then
		nox.log("deep_delve: an item not given back: " .. tostring(rec.type) .. " (" .. tostring(id) .. ")")
		return nil
	end
	items.setMeta(id, items.metaOf(rec))
	if wear then
		nox.equip(unit, id) -- (false and why when the class can't: then it's carried)
	end
	return id
end

-- Whether an item's pickup was a hand-over (once: the mark is taken).
function items.wasHanded(id: number): boolean
	local was = handed[id] == true
	handed[id] = nil
	return was
end

-- A record's item handed to a player's unit from the stash or the vendor: given as items.give
-- does; a weapon or armour its class may not use (which the game won't give, though any class may
-- pick anything up in Deep Delve) made at its feet and picked up by the game's own pickup, with
-- the pickup's refusals (a full pack, too much to carry), after which it's gone again. Its id, or
-- nil and why: then nothing was made, so the caller keeps what it had. `restore`: the record's own
-- gear put back on a character being set up (hero.setUp): taken into its inventory as the game's
-- give puts gear there, with no pickup to refuse it (a unit just in may not pick anything up yet).
function items.hand(unit: number, rec: any, restore: boolean?): (number?, string?)
	local ok, id = pcall(nox.giveItem, unit, rec.type, false, items.specOf(rec))
	if ok and id then
		items.setMeta(id, items.metaOf(rec))
		return id, nil
	end
	if not nox.present(unit) then
		return nil, "not now"
	end
	local x, y = nox.position(unit)
	local made, it = pcall(nox.create, rec.type, x, y, nil, items.specOf(rec))
	if not made or not it then
		nox.log("deep_delve: an item not handed over: " .. tostring(rec.type) .. " (" .. tostring(it) .. ")")
		return nil, "it couldn't be given"
	end
	if restore then
		if not pcall(nox.take, unit, it) then
			nox.delete(it)
			return nil, "it couldn't be given"
		end
		items.setMeta(it, items.metaOf(rec))
		return it, nil
	end
	handed[it] = true
	if not nox.pickUp(unit, it) then
		handed[it] = nil
		nox.delete(it)
		return nil, "you can't carry it"
	end
	items.setMeta(it, items.metaOf(rec))
	return it, nil
end

-- What an affix says ("+6% damage"): the loot's, or a stat only uniques and sets carry.
function items.affixText(a: any): string
	for _, def in loot.affixes do
		if def.id == a.id then
			return string.format(def.text, a.value)
		end
	end
	for _, def in uniqueData.stats :: { any } do
		if def.id == a.id then
			return string.format(def.text, a.value)
		end
	end
	return a.id
end

-- How an item reads in a line to its finder: "Rare Longsword (Steel, Fire2; +6% damage)"; a
-- unique or a set piece by its name: "Emberwake, the Kindled Staff (a unique LesserFireballWand;
-- +18 mana)".
function items.describe(rec: any): string
	local name = uniques.nameOf(rec)
	if name then
		local extra = {}
		for _, a in rec.affixes or {} do
			table.insert(extra, items.affixText(a))
		end
		return name .. " (" .. (if rec.unique then "a unique " else "a set piece, ") .. rec.type .. (if #extra > 0 then "; " .. table.concat(extra, ", ") else "") .. ")"
	end
	local parts = {}
	for _, slot in SLOTS do
		if rec[slot] then
			table.insert(parts, rec[slot])
		end
	end
	local extra = {}
	for _, a in rec.affixes or {} do
		table.insert(extra, items.affixText(a))
	end
	local words = (loot.words :: any)[rec.rarity or "normal"] or ""
	local text = (if words ~= "" then words .. " " else "") .. rec.type
	if #parts > 0 or #extra > 0 then
		text ..= " (" .. table.concat(parts, ", ")
		if #extra > 0 then
			text ..= (if #parts > 0 then "; " else "") .. table.concat(extra, ", ")
		end
		text ..= ")"
	end
	return text
end

return items
