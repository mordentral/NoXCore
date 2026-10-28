--!strict
-- Item records and the game's items (design 4.1): an item record is the thing type, its four
-- modifiers by slot (modifier.bin's names), durability and charges, plus Deep Delve's own: its
-- rarity, affixes and level (the area level it was made at, which its price goes by; none on
-- records older than version 3). The game's item carries the first part (A2); the rest is kept
-- here, by the item's id, for as long as the item is in the game (`meta`).

local loot = require("data/loot")

local items = {}

local SLOTS = { "effectiveness", "material", "primary", "secondary" }
items.SLOTS = SLOTS

-- Each live item's Deep Delve part: id -> { rarity, affixes }.
local meta: { [number]: any } = {}

function items.meta(id: number): any
	return meta[id]
end

-- An item's Deep Delve part kept; an item with affixes is watched for being put on and taken off
-- (onEquip: its wearer's numbers change with it).
function items.setMeta(id: number, m: any)
	meta[id] = m
	if m.affixes and #m.affixes > 0 then
		nox.watchEvents(id, nox.events.equip)
	end
end

function items.forget(id: number)
	meta[id] = nil
end

-- Every id forgotten (a new map: none of the old ones is in it).
function items.reset()
	table.clear(meta)
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
	items.setMeta(id, { rarity = rec.rarity or "normal", affixes = rec.affixes, level = rec.level })
	if wear then
		nox.equip(unit, id) -- (false and why when the class can't: then it's carried)
	end
	return id
end

-- What an affix says ("+6% damage").
function items.affixText(a: any): string
	for _, def in loot.affixes do
		if def.id == a.id then
			return string.format(def.text, a.value)
		end
	end
	return a.id
end

-- How an item reads in a line to its finder: "Rare Longsword (Steel, Fire2; +6% damage)".
function items.describe(rec: any): string
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
