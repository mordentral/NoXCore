--!strict
-- The stash (design 4.4): the town's chest opens the character's own stash (per character, by
-- decision; data/vendor.luau's tabs of slots), where items are kept in the record with their
-- modifiers, affixes and rarity, and gold too.
--
-- The record's part (store.luau's version 3): stash.tabs[tab][slot] = item record; gold.stashed.
--
-- An item is in one place at any time. A move is one step on the server: into the stash, the
-- item's record goes into the slot and the game item is deleted; out, the item is given and its
-- slot emptied, in the same call, and the record is written at once, whole. A write in between
-- can't happen (the game runs one hook at a time), and the record read back from a unit skips an
-- item being deleted (hero.snapshot), so a save, a quit or a map change straight after a move
-- keeps it where it went. A move is only taken from a hero set up in the map (never between
-- maps, when the unit doesn't carry the record's things).

local hero = require("hero")
local hud = require("hud")
local items = require("items")
local windows = require("windows")
local data = require("data/vendor")

local stash = {}

stash.TABS = data.stash.tabs
stash.SIZE = data.stash.size

-- The record's stash, made whole (every tab a table).
function stash.of(rec: any): any
	local s = rec.stash
	if type(s) ~= "table" then
		s = {}
		rec.stash = s
	end
	if type(s.tabs) ~= "table" then
		s.tabs = {}
	end
	for t = 1, stash.TABS do
		if type(s.tabs[t]) ~= "table" then
			s.tabs[t] = {}
		end
	end
	return s
end

local function freeSlot(tab: any): number?
	for slot = 1, stash.SIZE do
		if tab[slot] == nil then
			return slot
		end
	end
	return nil
end

-- The stash shown: tab `tab` (the one last shown, else 1).
function stash.view(h: any, tab: number?)
	local s = stash.of(h.record)
	local t = math.clamp(math.floor(tab or h.stashTab or 1), 1, stash.TABS)
	h.stashTab = t
	local list = {}
	for slot = 1, stash.SIZE do
		local rec = s.tabs[t][slot]
		if rec then
			local e = windows.entry(rec)
			e.s = slot
			table.insert(list, e)
		end
	end
	local counts = {}
	for i = 1, stash.TABS do
		local n = 0
		for _ in s.tabs[i] do
			n += 1
		end
		counts[i] = n
	end
	hud.parts(h.peer, "stash", windows.serial(), list, {
		tab = t,
		tabs = stash.TABS,
		size = stash.SIZE,
		stashed = h.record.gold.stashed or 0,
		counts = counts,
	})
end

local function ready(h: any): (boolean, string?)
	if not h or not h.ready or not nox.present(h.unit) then
		return false, "not now"
	end
	if not windows.near(h, "stash") then
		return false, "too far from the stash"
	end
	return true, nil
end

local function after(h: any, why: string)
	hero.save(h, why)
	stash.view(h)
	windows.sendCarried(h)
end

-- An item the hero carries into its stash (tab `tab`, else the one shown; slot `slot`, else the
-- first free one); whether it went, else why.
function stash.put(h: any, item: number, tab: number?, slot: number?): (boolean, string?)
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
	local s = stash.of(h.record)
	local t = math.clamp(math.floor(tab or h.stashTab or 1), 1, stash.TABS)
	local at = slot and math.floor(slot) or freeSlot(s.tabs[t])
	if not at or at < 1 or at > stash.SIZE or s.tabs[t][at] ~= nil then
		return false, "no room in that tab"
	end
	local rec = items.recordOf(item)
	if not rec then
		return false, "not an item"
	end
	rec.worn = nil
	-- (both in one step: the slot filled, the item gone)
	s.tabs[t][at] = rec
	items.forget(item)
	nox.delete(item)
	after(h, "stash")
	return true, nil
end

-- The item in tab `tab`, slot `slot` given back to the hero; whether it came, else why.
function stash.take(h: any, tab: number, slot: number): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local s = stash.of(h.record)
	local t = s.tabs[math.floor(tab)]
	local rec = t and t[math.floor(slot)]
	if not rec then
		return false, "nothing there"
	end
	local id = items.give(h.unit, rec, false)
	if not id then
		return false, "it couldn't be given"
	end
	t[math.floor(slot)] = nil
	after(h, "stash")
	return true, nil
end

-- All the gold the hero carries into the stash (`out`: all the stashed gold back out).
function stash.gold(h: any, out: boolean): (boolean, string?)
	local ok, why = ready(h)
	if not ok then
		return false, why
	end
	local g = h.record.gold
	local carried = nox.gold(h.unit)
	if out then
		nox.setGold(h.unit, carried + (g.stashed or 0), true)
		g.stashed = 0
	else
		nox.setGold(h.unit, 0, true)
		g.stashed = (g.stashed or 0) + carried
	end
	after(h, "stash")
	return true, nil
end

-- The chest touched: the stash's window.
function stash.touched(unit: number)
	local h = hero.byUnit(unit)
	if h then
		windows.open(h, "stash")
	end
end

-- A message from the stash's window: { op = "put", item, tab } | { op = "take", tab, slot } |
-- { op = "tab", tab } | { op = "deposit" } | { op = "withdraw" }.
function stash.message(h: any, v: any)
	if not windows.isOpen(h, "stash") then
		return
	end
	local ok, why = true, nil
	local item, tab, slot = tonumber(v.item), tonumber(v.tab), tonumber(v.slot)
	if v.op == "put" and item then
		ok, why = stash.put(h, item, tab)
	elseif v.op == "take" and tab and slot then
		ok, why = stash.take(h, tab, slot)
	elseif v.op == "tab" and tab then
		stash.view(h, tab)
	elseif v.op == "deposit" or v.op == "withdraw" then
		ok, why = stash.gold(h, v.op == "withdraw")
	end
	if not ok then
		windows.note(h, "stash", tostring(why))
	end
end

-- The stash's contents as lines, "tab slot type rarity modifiers" (for "dd stash").
function stash.lines(rec: any): { string }
	local s = stash.of(rec)
	local out = {}
	for t = 1, stash.TABS do
		for slot = 1, stash.SIZE do
			local r = s.tabs[t][slot]
			if r then
				local words = { tostring(t), tostring(slot), tostring(r.type), tostring(r.rarity or "normal"), tostring(r.effectiveness),
					tostring(r.material), tostring(r.primary), tostring(r.secondary) }
				table.insert(out, table.concat(words, " "))
			end
		end
	end
	table.insert(out, "gold " .. tostring(rec.gold.stashed or 0))
	return out
end

windows.register("stash", { station = "stash", view = stash.view })

return stash
