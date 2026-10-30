--!strict
-- World labels on NoX Core clients (ui/main.luau's nox.label, keyed by the object's net code):
-- ground loot's names, and named monsters' name plates.
--
-- Ground loot (design 4.3 and 6): each item the mod put on the ground is named over it in its
-- rarity's colour, shown while the player holds Alt (or always, as the character's option has it).
-- Each player's UI is sent the labels of the loot within numbers.labels.reach of its unit (at most
-- numbers.labels.most, nearest first) as one list, again only when that set changes. The UI sets
-- a label once, as its item comes into the list, kept while the item leaves the client's world and
-- comes back (UI API 4's keep), and clears it as the item leaves the list. A client can't tell an
-- item out of view from one gone for good, so the list is what ends a label: an item picked up or
-- gone is out of it at once (a pickup) or at the next beat (anything else), and an item further
-- than the reach too. That keeps a UI's labels to the list's most (well under the client's 128),
-- and a label never outlives its item long enough for its net code to come round on another.
--
-- Name plates (design 3.3): a rare's, a champion's or the act boss's name (any monster of the
-- mod's with a name) over its head in its tier's colour, always shown, Alt or not. Each player's
-- UI is sent the named monsters of the area as their own list ("plates": net code, name, colour),
-- again only when that set changes: a named one made, killed, charmed away or gone, so nothing
-- while they only move (the client draws a label where it draws its object). A dozen or so in an
-- area, so every one is sent while there are at most numbers.plates.most of them, the same list
-- to everyone; past that, the most nearest each player's unit. The plates' most and the loot's add
-- up to the client's 128 labels at most, and the UI sets a plate before a loot label.
--
-- One beat for both; it reads loot.luau's index of what lies on the ground and monsters.luau's of
-- the named monsters, never the world. A player whose unit isn't in play (dead) keeps the lists it
-- was sent, less the things gone.
--
-- A client lets every label go as it loads a new map (the new map numbers its objects afresh):
-- each list carries its map's number (`map`), new at each map change, so a UI knows the old
-- labels went; at the change each UI is told both lists are empty, and once its client is ready in
-- the new map it's sent what's there then.

local hero = require("hero")
local hud = require("hud")
local loot = require("loot")
local monsters = require("monsters")
local monsterData = require("data/monsters")
local numbers = require("data/numbers")

local labels = {}

local serial = 0
local plateSerial = 0
-- The map's number the lists carry (one more at each map change).
local mapSerial = 1
-- A beat asked for the next frame (a pickup, a named monster come or gone), or nil.
local soon: number? = nil
-- The plates every player is sent while the named monsters fit (nil: made again at the next
-- beat, the named set changed), and its signature.
local everyone: { any }? = nil
local everyoneSig = ""

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

-- ---- Name plates

-- A named monster's plate: { o = net code, t = its name, c = its tier's colour }.
local function plateOf(e: any): any
	local tier: any = (monsterData.tiers :: any)[e.tier]
	return { o = e.code, t = e.name, c = if tier then tier.colour else nil }
end

local function sendPlates(h: any, list: { any }, sig: string)
	h.plateSig = sig
	h.plateList = list
	plateSerial += 1
	hud.parts(h.peer, "plates", plateSerial, list, { map = mapSerial, offset = numbers.plates.offset })
end

-- The plates of the `most` named monsters nearest a hero's unit (more of them than fit).
local function nearestPlates(h: any, o: any): { any }
	local found: { any } = {}
	for id, e in monsters.named() do
		local x, y = nox.position(id)
		local dx, dy = x - o.x, y - o.y
		table.insert(found, { e = e, d = dx * dx + dy * dy })
	end
	table.sort(found, function(a: any, b: any): boolean
		if a.d ~= b.d then
			return a.d < b.d
		end
		return a.e.code < b.e.code
	end)
	local list: { any } = {}
	for i = 1, math.min(#found, numbers.plates.most) do
		table.insert(list, plateOf(found[i].e))
	end
	return list
end

local function platesTick()
	local named = monsters.named()
	-- (one gone without a death's word: forgotten, out of the index; the codes still there)
	local count = 0
	local there: { [number]: boolean } = {}
	for id, e in named do
		if not nox.present(id) then
			monsters.forget(id)
		else
			count += 1
			there[e.code] = true
		end
	end
	local fit = count <= numbers.plates.most
	if fit and not everyone then
		local list: { any } = {}
		for _, e in named do
			table.insert(list, plateOf(e))
		end
		table.sort(list, function(a: any, b: any): boolean
			return a.o < b.o
		end)
		everyone = list
		everyoneSig = signatureOf(list)
	end
	for _, h in hero.all() do
		if h.mapReady and nox.hasUi(h.peer) then
			local all = everyone
			if fit and all then
				if h.plateSig ~= everyoneSig then
					sendPlates(h, all, everyoneSig)
				end
			elseif h.ready and nox.present(h.unit) then
				local o = nox.object(h.unit)
				if o then
					local list = nearestPlates(h, o)
					local sig = signatureOf(list)
					if sig ~= h.plateSig then
						sendPlates(h, list, sig)
					end
				end
			elseif h.plateList and #h.plateList > 0 then
				-- (its unit out of play: what it was sent, less the monsters gone)
				local kept: { any } = {}
				for _, p in h.plateList do
					if there[p.o] then
						table.insert(kept, p)
					end
				end
				if #kept ~= #h.plateList then
					sendPlates(h, kept, signatureOf(kept))
				end
			end
		end
	end
end

-- ---- The beat

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
	platesTick()
end

local function tickSoon()
	if not soon then
		soon = nox.after(1, tick)
	end
end

-- The beat (the game's start); a named monster come or gone sends the plates at the next frame.
function labels.start()
	nox.every(numbers.labels.every, tick)
	monsters.onNamedChanged(function()
		everyone = nil
		tickSoon()
	end)
end

-- An item picked up (loot.luau has taken it out of the index): the lists without it sent at the
-- next frame, not the next beat.
function labels.changed()
	tickSoon()
end

-- A player's client ready in the map: its labels and plates sent again at the next beat.
function labels.resend(h: any)
	h.labelSig = nil
	h.plateSig = nil
end

-- A new map: the lists carry its number now; each player's UI told it has no labels or plates.
function labels.reset()
	mapSerial += 1
	everyone = nil
	everyoneSig = ""
	for _, h in hero.all() do
		if h.labelSig ~= nil and h.labelSig ~= "" then
			send(h, {}, "")
		end
		if h.plateSig ~= nil and h.plateSig ~= "" then
			sendPlates(h, {}, "")
		end
		h.labelSig = nil
		h.labelList = nil
		h.plateSig = nil
		h.plateList = nil
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

-- For "dd plates": the named monsters ("id code tier r,g,b name") and the codes last sent to `h`.
function labels.plateLines(h: any?): { string }
	local out = {}
	for id, e in monsters.named() do
		local p: any = plateOf(e)
		local c: any = p.c or {}
		local rgb = tostring(c[1]) .. "," .. tostring(c[2]) .. "," .. tostring(c[3])
		table.insert(out, table.concat({ tostring(id), tostring(e.code), tostring(e.tier), rgb, tostring(e.name) }, " "))
	end
	table.sort(out)
	if h then
		table.insert(out, "sent " .. tostring(h.plateSig or "-"))
	end
	return out
end

return labels
