--!strict
-- Things a player works by touching them (the town's gate and stations, the portal home, the
-- area's exit). The generator writes them into the map (a thing that never moves is drawn from
-- each client's copy of the map, so it can't be made later); here each is found at its spot once
-- the map has loaded, its touches sent to the mode's collide hook (nox.watchCollide) and from
-- there to its handler, by the thing's id.
--
-- A touch comes every frame a unit stands against the thing; `every` frames apart per unit is the
-- most a handler hears it (its first touch at once).
--
-- Two kinds of thing hear touches their own way (adopt's `how`):
--   * a step thing (a station's pad that opens a window: the waypoints' pentagram, the map table)
--     hears only a new step onto it: a touch after none from that unit for more than
--     numbers.touch.gap frames. Closed by its player, the window stays closed while it still
--     stands there.
--   * a way (a thing that takes a player elsewhere: the town's gate, the way home, the exit, the
--     portal's pentagram, the waypoints' pentagram) takes nobody for numbers.touch.grace frames
--     after that player arrived somewhere (touch.arrived: through a way, by a portal, a waypoint,
--     a map change, a respawn). One it touched in that time is held for it: it takes that player
--     only at a new step onto it (stepped off and come back). So a player who arrives walking
--     straight into the way it came by isn't sent back; a touch well after arriving goes at once.
--     A way the mod watches itself (the lair's gate: who stands in it, every few frames) asks
--     touch.passes with its own touch.way().

local numbers = require("data/numbers")

local touch = {}

local T = numbers.touch

local handlers: { [number]: any } = {}
-- The frame each unit last arrived somewhere (touch.arrived).
local arrivedAt: { [number]: number } = {}
-- This map's things of each type asked for: type -> ids (one walk of the world per type a map).
local ofType: { [string]: { number } } = {}

-- The map's thing of `thingType` standing at (x, y) (within a unit or two), or nil.
local function find(thingType: string, x: number, y: number): number?
	local ids = ofType[thingType]
	if not ids then
		ids = nox.objects(thingType)
		ofType[thingType] = ids
	end
	for _, id in ids do
		local ox, oy = nox.position(id)
		if math.abs(ox - x) < 2 and math.abs(oy - y) < 2 then
			return id
		end
	end
	return nil
end

-- A unit put somewhere new (through a way, a portal, a waypoint, into a new map, back from a
-- death): no way takes it for the grace (see the top).
function touch.arrived(unit: number)
	arrivedAt[unit] = nox.frame()
end

-- A way's own state: { seen (unit -> the frame of its last touch), held (unit -> true: it touched
-- the way during its arrival's grace, and is taken only at a new step onto it) }.
function touch.way(): any
	return { seen = {}, held = {} }
end

-- A touch of `w` (a step thing's or a way's state) by `unit` at frame `now`: whether it's a new step
-- onto it (no touch from that unit for more than the gap).
local function stepped(w: any, unit: number, now: number): boolean
	local seen = w.seen[unit]
	w.seen[unit] = now
	return seen == nil or now - seen > T.gap
end

-- Whether way `w` takes `unit` at this touch (`fresh`: a new step onto it).
local function lets(w: any, unit: number, now: number, fresh: boolean): boolean
	local at = arrivedAt[unit]
	if at and now >= at and now - at < T.grace then
		w.held[unit] = true
		return false
	end
	if w.held[unit] then
		if not fresh then
			return false
		end
		w.held[unit] = nil
	end
	return true
end

-- A way the mod watches itself (`w`: its touch.way()), `unit` found touching it now: whether it
-- takes the unit (looked at every few frames, no more than the gap apart, while it stands there).
function touch.passes(w: any, unit: number): boolean
	local now = nox.frame()
	return lets(w, unit, now, stepped(w, unit, now))
end

-- The map's thing of `thingType` at (x, y) set to call fn(unit, thing) when a player's unit
-- touches it (at most every `every` frames for one unit, default 30; `anyUnit`: a monster's
-- touch too); its id, nil when the map has none there. `how`: "way" (it takes a player elsewhere:
-- the arrival's grace, see the top), "step" (heard only at a new step onto it, `every` unused),
-- or both ("way step"); nil: neither.
function touch.adopt(thingType: string, x: number, y: number, fn: any, every: number?, anyUnit: boolean?, how: string?): number?
	local id = find(thingType, x, y)
	if not id then
		nox.log(string.format("deep_delve: no %s at %.1f, %.1f", thingType, x, y))
		return nil
	end
	nox.watchCollide(id)
	local kind = how or ""
	handlers[id] = {
		fn = fn,
		every = every or 30,
		last = {},
		anyUnit = anyUnit == true,
		way = string.find(kind, "way", 1, true) ~= nil,
		step = string.find(kind, "step", 1, true) ~= nil,
		seen = {},
		held = {},
	}
	return id
end

-- The mode's collide hook: a watched thing touched by `other`.
function touch.collide(thing: number, other: number?)
	local h = handlers[thing]
	if not h or not other then
		return
	end
	local o = nox.object(other)
	if not o or not o.alive or not (o.player or (h.anyUnit and o.maxHealth > 0)) then
		return
	end
	local now = nox.frame()
	if h.way or h.step then
		local fresh = stepped(h, other, now)
		if h.way and not lets(h, other, now, fresh) then
			return
		end
		if h.step then
			if not fresh then
				return
			end
			h.fn(other, thing)
			return
		end
	end
	local last = h.last[other]
	if last and now - last < h.every then
		return
	end
	h.last[other] = now
	h.fn(other, thing)
end

-- A new map: the old things are gone with it (and the arrivals past their grace).
function touch.reset()
	table.clear(handlers)
	table.clear(ofType)
	local now = nox.frame()
	for unit, at in arrivedAt do
		if now < at or now - at >= T.grace then
			arrivedAt[unit] = nil
		end
	end
end

return touch
