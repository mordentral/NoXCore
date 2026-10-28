--!strict
-- Things a player works by touching them (the town's gate and stations, the portal home, the
-- area's exit). The generator writes them into the map (a thing that never moves is drawn from
-- each client's copy of the map, so it can't be made later); here each is found at its spot once
-- the map has loaded, its touches sent to the mode's collide hook (nox.watchCollide) and from
-- there to its handler, by the thing's id.
--
-- A touch comes every frame a unit stands against the thing; `every` frames apart per unit is the
-- most a handler hears it (its first touch at once).

local touch = {}

local handlers: { [number]: any } = {}
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

-- The map's thing of `thingType` at (x, y) set to call fn(unit, thing) when a player's unit
-- touches it (at most every `every` frames for one unit, default 30; `anyUnit`: a monster's
-- touch too); its id, nil when the map has none there.
function touch.adopt(thingType: string, x: number, y: number, fn: any, every: number?, anyUnit: boolean?): number?
	local id = find(thingType, x, y)
	if not id then
		nox.log(string.format("deep_delve: no %s at %.1f, %.1f", thingType, x, y))
		return nil
	end
	nox.watchCollide(id)
	handlers[id] = { fn = fn, every = every or 30, last = {}, anyUnit = anyUnit == true }
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
	local last = h.last[other]
	if last and now - last < h.every then
		return
	end
	h.last[other] = now
	h.fn(other, thing)
end

-- A new map: the old things are gone with it.
function touch.reset()
	table.clear(handlers)
	table.clear(ofType)
end

return touch
