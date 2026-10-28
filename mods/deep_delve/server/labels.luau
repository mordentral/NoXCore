--!strict
-- Ground loot's labels (design 4.3 and 6): each item the mod put on the ground is named over it
-- in its rarity's colour on NoX Core clients, shown while the player holds Alt (ui/main.luau's
-- nox.label, keyed by the item's net code).
--
-- Each player's UI is sent the labels of the loot within numbers.labels.reach of its unit (at most
-- numbers.labels.most, nearest first) as one list, again only when that set changes: a client
-- keeps a label only while its item is in its own world, so a far item's would be dropped anyway,
-- and the list stays small. One beat for everyone; it reads loot.luau's index of what lies on the
-- ground, never the world.
--
-- A client lets every label go as it loads a new map (the new map numbers its objects afresh):
-- at a map change each UI is told the list is empty, and once its client is ready in the new map
-- it's sent what's near it then.

local hero = require("hero")
local hud = require("hud")
local loot = require("loot")
local numbers = require("data/numbers")

local labels = {}

local serial = 0

-- The labels near a hero: { { o = net code, t = text, r = rarity } } nearest first, and the set's
-- signature (its codes in order).
local function nearOf(h: any): ({ any }, string)
	local o = nox.object(h.unit)
	if not o then
		return {}, ""
	end
	local reach = numbers.labels.reach
	local found: { any } = {}
	for _, g in loot.ground() do
		local dx, dy = g.x - o.x, g.y - o.y
		local d = dx * dx + dy * dy
		if d <= reach * reach then
			table.insert(found, { g = g, d = d })
		end
	end
	table.sort(found, function(a: any, b: any): boolean
		if a.d ~= b.d then
			return a.d < b.d
		end
		return a.g.code < b.g.code
	end)
	local list: { any } = {}
	local codes: { number } = {}
	for i, f in found do
		if i > numbers.labels.most then
			break
		end
		table.insert(list, { o = f.g.code, t = f.g.text, r = f.g.rarity })
		table.insert(codes, f.g.code)
	end
	table.sort(codes)
	local words: { string } = {}
	for _, c in codes do
		table.insert(words, tostring(c))
	end
	return list, table.concat(words, " ")
end

local function tick()
	local ground = loot.ground()
	-- (an item gone without a pickup's word: out of the index)
	for id in ground do
		if not nox.present(id) then
			ground[id] = nil
		end
	end
	for _, h in hero.all() do
		if h.mapReady and h.ready and nox.present(h.unit) and nox.hasUi(h.peer) then
			local list, sig = nearOf(h)
			if sig ~= h.labelSig then
				h.labelSig = sig
				serial += 1
				hud.parts(h.peer, "labels", serial, list)
			end
		end
	end
end

-- The beat (the game's start).
function labels.start()
	nox.every(numbers.labels.every, tick)
end

-- A player's client ready in the map: its labels sent again at the next beat.
function labels.resend(h: any)
	h.labelSig = nil
end

-- A new map: each player's UI told it has no labels now.
function labels.reset()
	for _, h in hero.all() do
		if h.labelSig ~= nil and h.labelSig ~= "" then
			serial += 1
			hud.parts(h.peer, "labels", serial, {})
		end
		h.labelSig = nil
	end
end

-- For "dd labels": the ground's labels ("id code rarity text") and the codes last sent to `h`.
function labels.lines(h: any?): { string }
	local out = {}
	for id, g in loot.ground() do
		table.insert(out, table.concat({ tostring(id), tostring(g.code), tostring(g.rarity), tostring(g.text) }, " "))
	end
	table.sort(out)
	if h then
		table.insert(out, "sent " .. tostring(h.labelSig or "-"))
	end
	return out
end

return labels
