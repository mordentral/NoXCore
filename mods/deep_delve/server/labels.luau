--!strict
-- Ground loot's labels (design 4.3 and 6): each item the mod put on the ground is named over it
-- in its rarity's colour on NoX Core clients, shown while the player holds Alt (ui/main.luau's
-- nox.label, keyed by the item's net code).
--
-- Each player's UI is sent the labels of the loot within numbers.labels.reach of its unit (at most
-- numbers.labels.most, nearest first) as one list, again only when that set changes. The UI sets
-- a label once, as its item comes into the list, kept while the item leaves the client's world and
-- comes back (UI API 4's keep), and clears it as the item leaves the list. A client can't tell an
-- item out of view from one gone for good, so the list is what ends a label: an item picked up or
-- gone is out of it at once (a pickup) or at the next beat (anything else), and an item further
-- than the reach too. That keeps a UI's labels to the list's most (well under the client's 128),
-- and a label never outlives its item long enough for its net code to come round on another.
--
-- One beat for everyone; it reads loot.luau's index of what lies on the ground, never the world.
-- A player whose unit isn't in play (dead) keeps the list it was sent, less the items gone.
--
-- A client lets every label go as it loads a new map (the new map numbers its objects afresh):
-- each list carries its map's number (`map`), new at each map change, so a UI knows the old
-- labels went; at the change each UI is told the list is empty, and once its client is ready in
-- the new map it's sent what's near it then.

local hero = require("hero")
local hud = require("hud")
local loot = require("loot")
local numbers = require("data/numbers")

local labels = {}

local serial = 0
-- The map's number the lists carry (one more at each map change).
local mapSerial = 1
-- A beat asked for the next frame (a pickup), or nil.
local soon: number? = nil

-- A list's signature: its codes in order.
local function signatureOf(list: { any }): string
	local codes: { number } = {}
	for _, l in list do
		table.insert(codes, l.o)
	end
	table.sort(codes)
	local words: { string } = {}
	for _, c in codes do
		table.insert(words, tostring(c))
	end
	return table.concat(words, " ")
end

-- The labels near a hero: { { o = net code, t = text, r = rarity } } nearest first.
local function nearOf(h: any): { any }
	local o = nox.object(h.unit)
	if not o then
		return {}
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
	for i, f in found do
		if i > numbers.labels.most then
			break
		end
		table.insert(list, { o = f.g.code, t = f.g.text, r = f.g.rarity })
	end
	return list
end

local function send(h: any, list: { any }, sig: string)
	h.labelSig = sig
	h.labelList = list
	serial += 1
	hud.parts(h.peer, "labels", serial, list, { map = mapSerial })
end

local function tick()
	soon = nil
	local ground = loot.ground()
	-- (an item gone without a pickup's word: out of the index; the codes still lying there)
	local lying: { [number]: boolean } = {}
	for id, g in ground do
		if not nox.present(id) then
			ground[id] = nil
		else
			lying[g.code] = true
		end
	end
	for _, h in hero.all() do
		if h.mapReady and nox.hasUi(h.peer) then
			if h.ready and nox.present(h.unit) then
				local list = nearOf(h)
				local sig = signatureOf(list)
				if sig ~= h.labelSig then
					send(h, list, sig)
				end
			elseif h.labelList and #h.labelList > 0 then
				-- (its unit out of play: what it was sent, less the items gone)
				local kept: { any } = {}
				for _, l in h.labelList do
					if lying[l.o] then
						table.insert(kept, l)
					end
				end
				if #kept ~= #h.labelList then
					send(h, kept, signatureOf(kept))
				end
			end
		end
	end
end

-- The beat (the game's start).
function labels.start()
	nox.every(numbers.labels.every, tick)
end

-- An item picked up (loot.luau has taken it out of the index): the lists without it sent at the
-- next frame, not the next beat.
function labels.changed()
	if not soon then
		soon = nox.after(1, tick)
	end
end

-- A player's client ready in the map: its labels sent again at the next beat.
function labels.resend(h: any)
	h.labelSig = nil
end

-- A new map: the lists carry its number now; each player's UI told it has no labels.
function labels.reset()
	mapSerial += 1
	for _, h in hero.all() do
		if h.labelSig ~= nil and h.labelSig ~= "" then
			send(h, {}, "")
		end
		h.labelSig = nil
		h.labelList = nil
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
