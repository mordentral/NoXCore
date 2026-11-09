--!strict
-- The bigger windows on a player's UI (ui/main.luau draws them): the stash and the vendor, opened
-- by touching their station in town, and the passive tree and the character sheet, opened from
-- the player's small window. Which are open is kept on the hero (`open`); each window's module
-- (stash.luau, vendor.luau, treeview.luau, sheet.luau) registers how it's shown.
--
-- The stash and the vendor both show what the player carries (not what it wears): one list, the
-- gear first (not the secondary weapon) and then the potions and supplies, the oldest first in
-- each; plain potions and supplies
-- of one type stacked three to an entry with their count and ids (windows.grid). With the vendor's price
-- for each while the vendor is open (and its repair's, with the list of the pieces worn or carried
-- that need one).
--
-- One beat runs while anyone has a window open: a station window closes when its player walks
-- away from the station, a changed inventory is shown again, and an open sheet or tree is kept
-- current. It looks only at the heroes with a window open.

local hero = require("hero")
local hud = require("hud")
local items = require("items")
local tips = require("tips")
local town = require("town")
local vendorData = require("data/vendor")

local windows = {}

-- A window's module: { station = the town spot it needs (nil: none), view = fn(h) sends it,
-- beat = fn(h) (optional) keeps it current, price and reroll = fn(rec), repairCost = fn(id),
-- withRepair = fn(lines, cost) and repairs = fn(h) (the vendor's) }.
local kinds: { [string]: any } = {}
local serial = 0
local beat: number? = nil

function windows.register(name: string, def: any)
	if def.beat == nil then
		def.beat = function(_h: any) end
	end
	kinds[name] = def
end

-- A new serial for a list sent in parts (hud.parts).
function windows.serial(): number
	serial += 1
	return serial
end

function windows.isOpen(h: any, name: string): boolean
	return h.open ~= nil and h.open[name] == true
end

-- Whether the hero stands within reach of a town spot ("stash", "vendor", "healer").
function windows.near(h: any, spot: string): boolean
	local at = town.spot(spot)
	if not at or not h.ready or not nox.present(h.unit) then
		return false
	end
	local x, y = nox.position(h.unit)
	return math.sqrt((x - at.x) ^ 2 + (y - at.y) ^ 2) <= vendorData.reach
end

-- A short line to the window's player (shown in the window).
function windows.note(h: any, window: string, text: string)
	hud.send(h.peer, "note:" .. window, { kind = "note", window = window, text = text })
end

-- What the hero carries, not worn and not going: { { id, rec, facts } } in its inventory's order
-- (facts: nox.item's).
function windows.carried(h: any): { any }
	local out = {}
	if not nox.present(h.unit) then
		return out
	end
	for _, it in nox.inventory(h.unit) do
		local o = nox.object(it.id)
		local f = nox.item(it.id)
		if o and not o.pendingDelete and f and not f.worn then
			local rec = items.recordOf(it.id)
			if rec then
				rec.worn = nil
				table.insert(out, { id = it.id, rec = rec, facts = f })
			end
		end
	end
	return out
end

-- The types stacked in the carried grid: the vendor's potions and supplies (a plain one's).
local stackable: { [string]: boolean } = {}
for _, p in vendorData.potions do
	stackable[p.type] = true
end
for _, p in vendorData.supplies do
	stackable[p.type] = true
end

-- What a carried record stacks by: nil for one shown alone (gear, or a potion or a supply with
-- anything that makes it different: a rarity, affixes, modifiers, a unique's or a set's id); else
-- its type, durability and charges (a quiver's arrows: a full one never stacks with a used one).
function windows.stackKey(rec: any): string?
	if not stackable[rec.type] or (rec.rarity or "normal") ~= "normal" or rec.unique or rec.set then
		return nil
	end
	if rec.affixes and #rec.affixes > 0 then
		return nil
	end
	for _, slot in items.SLOTS do
		if rec[slot] then
			return nil
		end
	end
	return rec.type .. "/" .. tostring(rec.durability) .. "/" .. tostring(rec.charges)
end

-- The carried grid's entries: { { id, ids, rec, facts } }, the gear first, then the potions and
-- supplies, each part the oldest first (the inventory's own order is the newest first); the plain
-- potions and supplies of one type (windows.stackKey) one entry of at most `stackMax` (more start
-- another, after it), `ids` theirs (the oldest first, `id` the first of them), at the place of the
-- oldest. The secondary weapon isn't in it (it's neither sold nor stashed: windows.keptBack).
function windows.grid(h: any): { any }
	local out, stacks = {}, {}
	local byKey: { [string]: any } = {}
	local list = windows.carried(h)
	local second = if #list > 0 then nox.secondaryWeapon(h.unit) else nil
	for i = #list, 1, -1 do
		local c = list[i]
		local key = windows.stackKey(c.rec)
		if c.id == second then
			continue
		elseif key == nil then
			table.insert(out, { id = c.id, ids = { c.id }, rec = c.rec, facts = c.facts })
		else
			local s = byKey[key]
			if s and #s.ids < vendorData.stackMax then
				table.insert(s.ids, c.id)
			else
				s = { id = c.id, ids = { c.id }, rec = c.rec, facts = c.facts }
				byKey[key] = s
				table.insert(stacks, s)
			end
		end
	end
	for _, s in stacks do
		table.insert(out, s)
	end
	return out
end

-- An item's entry for a window, shown to hero `h`: { t = type, r = rarity, tl = its tooltip's
-- lines (tips.luau) }; `facts` a live item's (nox.item's), nil for a record (stashed, stocked).
function windows.entry(h: any, rec: any, facts: any?): any
	return { t = rec.type, r = rec.rarity or "normal", tl = tips.lines(rec, facts, h) }
end

-- The inventory's ids, its secondary weapon and its gold as one string (whether it changed since
-- it was shown).
local function signature(h: any): string
	if not nox.present(h.unit) then
		return ""
	end
	local ids = {}
	for _, it in nox.inventory(h.unit) do
		table.insert(ids, tostring(it.id))
	end
	return table.concat(ids, ",") .. "/" .. tostring(nox.secondaryWeapon(h.unit)) .. "/" .. tostring(nox.gold(h.unit))
end

-- Why a carried item may not leave by the stash or the vendor (nil: it may): it's the hero's
-- secondary weapon (by decision 2026-11-08, issue #14).
function windows.keptBack(h: any, item: number): string?
	if nox.secondaryWeapon(h.unit) == item then
		return "that's your secondary weapon"
	end
	return nil
end

-- What the hero carries, to its UI (with the vendor's prices while the vendor is open): the grid's
-- entries (windows.grid), each { i = its id, t, r, tl }, a stack's with n = its count and
-- ids = its ids too (i the first of them: a click sells or stashes that one).
function windows.sendCarried(h: any)
	local list = {}
	local priced = windows.isOpen(h, "vendor") and kinds.vendor
	for _, c in windows.grid(h) do
		local e = windows.entry(h, c.rec, c.facts)
		e.i = c.id
		if #c.ids > 1 then
			e.n = #c.ids
			e.ids = c.ids
		end
		if priced then
			e.p = kinds.vendor.price(c.rec)
			if c.rec.rarity == "magic" or c.rec.rarity == "rare" then
				e.q = kinds.vendor.reroll(c.rec)
			end
			-- (its repair's price, when it needs one: a line of its tooltip)
			local cost = kinds.vendor.repairCost(c.id)
			if cost then
				e.tl = kinds.vendor.withRepair(e.tl, cost)
			end
		end
		table.insert(list, e)
	end
	h.shownCarried = signature(h)
	hud.parts(h.peer, "carried", windows.serial(), list, { gold = if nox.present(h.unit) then nox.gold(h.unit) else 0 })
	-- (the vendor's: the pieces worn or carried that need a repair)
	if priced then
		kinds.vendor.repairs(h)
	end
end

local function anyOpen(): boolean
	for _, h in hero.all() do
		if h.open and next(h.open) ~= nil then
			return true
		end
	end
	return false
end

local function tick()
	for _, h in hero.all() do
		local open: { [string]: boolean }? = h.open
		if open and next(open) ~= nil then
			for name: string in table.clone(open) do
				local def = kinds[name]
				if def and def.station and not windows.near(h, def.station) then
					windows.close(h, name, true)
				end
			end
			if (open.stash or open.vendor) and h.ready and signature(h) ~= h.shownCarried then
				windows.sendCarried(h)
			end
			for name: string in open do
				local def = kinds[name]
				if def then
					def.beat(h)
				end
			end
		end
	end
	if not anyOpen() and beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

-- A window opened on the hero's UI and shown (a station's only within reach of it).
function windows.open(h: any, name: string): boolean
	local def = kinds[name]
	if not def or not h.ready then
		return false
	end
	if def.station and not windows.near(h, def.station) then
		return false
	end
	h.open = h.open or {}
	local was = h.open[name] == true
	h.open[name] = true
	if not was then
		hud.send(h.peer, "win:" .. name, { kind = "open", window = name })
	end
	if def.station then
		windows.sendCarried(h)
	end
	def.view(h)
	if not beat then
		beat = nox.every(15, tick)
	end
	return true
end

-- A window closed (`tell`: its UI told too; else the UI closed it itself).
function windows.close(h: any, name: string, tell: boolean?)
	if h.open then
		h.open[name] = nil
	end
	if tell then
		hud.send(h.peer, "win:" .. name, { kind = "close", window = name })
	end
end

-- A new map: every station window closed (its player is somewhere else now).
function windows.reset()
	for _, h in hero.all() do
		for name in table.clone(h.open or {}) do
			local def = kinds[name]
			if def and def.station then
				windows.close(h, name, true)
			end
		end
	end
end

return windows
