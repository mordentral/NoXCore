--!strict
-- The pads' looks on the floor: a glowing ring drawn flat under each pentagram that does
-- something (nox.decal), so the ways read apart at a glance. Waypoints (the town's waypoints
-- pentagram, an area's waypoint) get the cyan star, turning slowly; the town's portal pentagram
-- and every portal a player opens out in the field get the violet swirl, turning faster the
-- other way and pulsing harder. Only the look: the pentagrams' own touches work as they did.
--
-- A map's decals go with it (the host clears them at each load), so the pads are dressed again
-- as each area is entered, and a field portal's ring comes and goes with the portal. A map holds
-- at most 64; past that a pad simply has none (nothing here needs one to work).

local numbers = require("data/numbers")

local decals = {}

-- The decals this map holds: id -> { kind, x, y } (for "dd decals").
local placed: { [number]: any } = {}

-- A pad's ring of `kind` ("waypoint" or "portal", numbers.decals') at (x, y); its id, or nil
-- when the map holds its 64 already.
function decals.put(kind: string, x: number, y: number): number?
	local k: any = (numbers.decals :: any)[kind]
	if not k then
		return nil
	end
	local size: number, spin: number, amount: number, period: number = k.size, k.spin, k.pulse, k.period
	local id = nox.decal(tostring(k.asset), x, y, { size = size, glow = true, spin = spin, pulse = { amount = amount, period = period } })
	if id then
		placed[id] = { kind = kind, x = x, y = y }
	end
	return id
end

-- A ring gone (a field portal closed); nil: nothing.
function decals.remove(id: number?)
	if id and placed[id] then
		placed[id] = nil
		nox.removeDecal(id)
	end
end

-- A map's town and area entered (`spots`: the generator's): its pads dressed.
function decals.place(spots: any)
	decals.put("portal", spots.town.portal.x, spots.town.portal.y)
	decals.put("waypoint", spots.town.waypoints.x, spots.town.waypoints.y)
	if spots.waypoint then
		decals.put("waypoint", spots.waypoint.x, spots.waypoint.y)
	end
end

-- The map's decals as lines, "kind x y" sorted (for "dd decals").
function decals.lines(): { string }
	local out = {}
	for _, d in placed do
		table.insert(out, string.format("%s %.1f %.1f", tostring(d.kind), tonumber(d.x) or 0, tonumber(d.y) or 0))
	end
	table.sort(out)
	return out
end

-- A new map: the old one's decals are gone with it (the host cleared them).
function decals.reset()
	placed = {}
end

return decals
