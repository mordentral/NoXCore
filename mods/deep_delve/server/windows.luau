--!strict
-- The bigger windows on a player's UI (ui/main.luau draws them): the stash and the vendor, opened
-- by touching their station in town, and the passive tree and the character sheet, opened from
-- the player's small window. Which are open is kept on the hero (`open`); each window's module
-- (stash.luau, vendor.luau, treeview.luau, sheet.luau) registers how it's shown.
--
-- The stash and the vendor both show what the player carries (not what it wears): one list,
-- with the vendor's price for each while the vendor is open.
--
-- One beat runs while anyone has a window open: a station window closes when its player walks
-- away from the station, a changed inventory is shown again, and an open sheet or tree is kept
-- current. It looks only at the heroes with a window open.

local hero = require("hero")
local hud = require("hud")
local items = require("items")
local town = require("town")
local vendorData = require("data/vendor")

local windows = {}

-- A window's module: { station = the town spot it needs (nil: none), view = fn(h) sends it,
-- beat = fn(h) (optional) keeps it current, price and reroll = fn(rec) (the vendor's) }.
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

-- What the hero carries, not worn and not going: { { id, rec } } in its inventory's order.
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
				table.insert(out, { id = it.id, rec = rec })
			end
		end
	end
	return out
end

-- An item's line for a window: { t = type, r = rarity, d = what it is (at most 160 letters) }.
function windows.entry(rec: any): any
	local d = items.describe(rec)
	if rec.level then
		d ..= ", level " .. tostring(rec.level)
	end
	if #d > 160 then
		d = string.sub(d, 1, 157) .. "..."
	end
	return { t = rec.type, r = rec.rarity or "normal", d = d }
end

-- The inventory's ids as one string (whether it changed since it was shown).
local function signature(h: any): string
	if not nox.present(h.unit) then
		return ""
	end
	local ids = {}
	for _, it in nox.inventory(h.unit) do
		table.insert(ids, tostring(it.id))
	end
	return table.concat(ids, ",") .. "/" .. tostring(nox.gold(h.unit))
end

-- What the hero carries, to its UI (with the vendor's prices while the vendor is open).
function windows.sendCarried(h: any)
	local list = {}
	local priced = windows.isOpen(h, "vendor") and kinds.vendor
	for _, c in windows.carried(h) do
		local e = windows.entry(c.rec)
		e.i = c.id
		if priced then
			e.p = kinds.vendor.price(c.rec)
			if c.rec.rarity == "magic" or c.rec.rarity == "rare" then
				e.q = kinds.vendor.reroll(c.rec)
			end
		end
		table.insert(list, e)
	end
	h.shownCarried = signature(h)
	hud.parts(h.peer, "carried", windows.serial(), list, { gold = if nox.present(h.unit) then nox.gold(h.unit) else 0 })
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
