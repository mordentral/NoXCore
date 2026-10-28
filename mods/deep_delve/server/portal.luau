--!strict
-- The town portal: back to town at any time, and back out to where you were.
--
--   * Opening: the Town portal button in the player's Deep Delve window (ui/main.luau sends
--     { kind = "portal" }), or saying "portal". Out in the area, the portal opens where the player
--     stands and the player goes home at once. In town it does nothing.
--   * Where it stands: violet sparks every so often, so it reads as a way through. It's the
--     player's own, one at a time; a new one moves it.
--   * Back out: the town's portal pentagram (violet, east of the fountain) takes its owner back
--     to the portal, which closes behind them.
--   * It lasts until it's used or the map changes (a portal leads only within its map: the town
--     is in every map, the portal's place is in this one).
--
-- The open portal is kept on the hero (hero.luau's `portal`); one shared beat sparks them all.

local hero = require("hero")
local hud = require("hud")
local town = require("town")
local touch = require("touch")
local numbers = require("data/numbers")

local portal = {}

local beat: number? = nil

local function sparks()
	local any = false
	for _, h in hero.all() do
		local p = h.portal
		if p then
			nox.effect(numbers.portal.sparks, p.x, p.y)
			any = true
		end
	end
	if not any and beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

-- A hero opens a town portal; what came of it ("opened", "in town", "not now").
function portal.open(h: any): string
	if not h or not h.ready then
		return "not now"
	end
	local o = nox.object(h.unit)
	if not o or not o.alive then
		return "not now"
	end
	if town.contains(o.x, o.y) then
		town.say(h.unit, "You're in town already.")
		return "in town"
	end
	h.portal = { x = o.x, y = o.y }
	nox.effect(0x89, o.x, o.y) -- (MSG_FX_TELEPORT)
	town.sendHome(h.unit)
	nox.tell(h.unit, "Your portal stays open where you stood: the violet pentagram in town takes you back.")
	if not beat then
		beat = nox.every(numbers.portal.every, sparks)
	end
	hud.hero(h)
	return "opened"
end

-- The town's portal pentagram touched: its owner back out through its portal.
local function back(unit: number)
	local h = hero.byUnit(unit)
	if not h then
		return
	end
	local p = h.portal
	if not p then
		town.say(unit, "No portal of yours is open. Open one out in the field (the Town portal button, or say \"portal\").")
		return
	end
	h.portal = nil
	local x, y = nox.clearPointNear(p.x, p.y, 40)
	nox.setPosition(unit, x, y)
	nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
	hud.hero(h)
end

-- A map's town: its portal pentagram set to work.
function portal.place(townSpots: any)
	touch.adopt("TeleportPentagram", townSpots.portal.x, townSpots.portal.y, back, 45)
end

-- A new map: every portal closed (hero.mapChanging forgets them on the heroes).
function portal.reset()
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

return portal
