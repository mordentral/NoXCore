--!strict
-- The town's stations (design 3.1), which the generator writes into each map at its town spots:
--   * the gate: an arch that takes a player out to the area;
--   * the healer: a fountain that fills health and mana (and where a passive tree is respecced);
--   * the stash's chest and the vendor's anvil: touched, their windows open (stash.luau,
--     vendor.luau);
--   * the portal's and the waypoints' pentagrams (portal.luau and waypoints.luau work them);
-- and where the fallen come back (the town's middle).

local touch = require("touch")

local town = {}

local spots: any = nil

-- The town's reach from its middle, |dx| + |dy| in world units: its walls' (gen/town.luau's
-- RADIUS, 12 cells). The area's floor is never nearer than six cells past them.
local REACH = 12 * 23

-- Whether a point is in the town.
function town.contains(x: number, y: number): boolean
	if not spots then
		return false
	end
	return math.abs(x - spots.centre.x) + math.abs(y - spots.centre.y) <= REACH
end

-- A short line to a unit, not more than once in three seconds for the same words.
local lastLine: { [number]: { text: string, frame: number } } = {}
local function say(unit: number, text: string)
	local now = nox.frame()
	local last = lastLine[unit]
	if last and last.text == text and now - last.frame < 90 then
		return
	end
	lastLine[unit] = { text = text, frame = now }
	nox.tell(unit, text)
end
town.say = say

local function heal(unit: number)
	local o = nox.object(unit)
	if not o then
		return
	end
	nox.setPlayerHealth(unit, o.maxHealth, o.maxHealth)
	local _, most = nox.mana(unit)
	nox.setMana(unit, most, most)
	nox.effect(0x81, o.x, o.y) -- (MSG_FX_BLUE_SPARKS)
	say(unit, "The waters of the Waystation make you whole.")
end

-- The map's stations (the generator wrote them at its town spots) set to work. `out(unit)`: the
-- gate's way to the area; `stash(unit)` and `vendor(unit)`: the chest's and the anvil's windows
-- opened.
function town.place(townSpots: any, out: any, stash: any, vendor: any)
	spots = townSpots
	table.clear(lastLine)
	touch.adopt(spots.gate.type, spots.gate.x, spots.gate.y, out)
	touch.adopt("Fountain", spots.healer.x, spots.healer.y, heal)
	touch.adopt("Chest1", spots.stash.x, spots.stash.y, stash)
	touch.adopt("Anvil1", spots.vendor.x, spots.vendor.y, vendor)
end

-- A station's spot in this map's town ("stash", "vendor", "healer", ...), or nil.
function town.spot(name: string): any
	return spots and spots[name]
end

-- A point in town for a unit to come back at: south of the fountain (clear of it), a little apart.
function town.respawnPoint(): (number?, number?)
	if not spots then
		return nil, nil
	end
	local c = spots.centre
	return nox.clearPointNear(c.x + nox.random(-30, 30), c.y + 92, 30)
end

-- A unit sent home (the way back from the area's start, a town portal).
function town.sendHome(unit: number)
	local x, y = town.respawnPoint()
	if x and y then
		nox.setPosition(unit, x, y)
		nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
	end
end

return town
