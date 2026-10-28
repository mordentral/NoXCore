--!strict
-- Waypoints. Some areas have one (data/acts.luau): a lit teleport pentagram in a side room off the
-- route. Each character keeps the waypoints it has found (progress.luau, per tier and act).
--
--   * Found: by coming within numbers.waypoints.reach of it (looked for a few times a second, for
--     the heroes only), by arriving through it, or by arriving in town from it.
--   * Out in the area -> town: the waypoint is a real pentagram written into the map, leading to
--     the town's receiving pentagram; the game itself sends whoever stands on it (a pentagram
--     links places in one map, and the town is in every map).
--   * Town -> a waypoint: the town's waypoints pentagram (cyan) offers the waypoints its
--     toucher has found (nox.choice). This area's: a step across the map, at once. Another
--     area's: the whole game goes there (a map change, that area generated into the other map
--     tag), arriving beside its waypoint. A party's players must all be in town for that (a party
--     vote comes with party play); alone, it's at once.
--   * The town's receiving pentagram watches what lands on it: a monster that followed a player
--     through is sent back to the waypoint it came by.
--
-- The town's waypoints pentagram is the mod's (watched, leading nowhere by itself) because only
-- the mod knows which waypoints a character has found.

local hero = require("hero")
local hud = require("hud")
local monsters = require("monsters")
local progress = require("progress")
local town = require("town")
local touch = require("touch")
local numbers = require("data/numbers")

local waypoints = {}

-- This map's waypoint: { act, index, title, x, y, arrive }, or nil.
local here: any = nil
local beat: number? = nil
-- What each peer was offered: peer -> { list = area indices, frame }.
local offered: { [number]: any } = {}
-- areas.luau's own (handed in by place: it requires this module).
local areas: any = nil

local OFFER_AGAIN = 30 * 10

local function found(h: any)
	if not here or not progress.addWaypoint(h.record, here.act, here.index) then
		return
	end
	nox.effect(0x83, here.x, here.y) -- (MSG_FX_CYAN_SPARKS)
	if nox.present(h.unit) then
		nox.tell(h.unit, "Waypoint found: " .. here.title .. ". Its pentagram leads to town; the cyan pentagram in town leads back.")
	end
	hero.save(h, "waypoint found")
	hud.hero(h)
end

-- The heroes near this map's waypoint find it.
local function look()
	if not here then
		return
	end
	local reach = numbers.waypoints.reach
	for _, h in hero.all() do
		if h.ready and nox.present(h.unit) and not progress.hasWaypoint(h.record, here.act, here.index) then
			local x, y = nox.position(h.unit)
			local dx, dy = x - here.x, y - here.y
			if dx * dx + dy * dy <= reach * reach then
				found(h)
			end
		end
	end
end

-- A hero to an area's waypoint (one it has found): this area's at once, another's by a map
-- change. Whether it went (else why).
function waypoints.go(h: any, index: number): (boolean, string?)
	local current = areas.current()
	if index == current.index then
		local at = current.spots and current.spots.wayArrive
		if not at then
			return false, "no waypoint here"
		end
		local x, y = nox.clearPointNear(at.x, at.y, 30)
		nox.setPosition(h.unit, x, y)
		nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
		return true, nil
	end
	-- Another area: the game goes, so everyone else must be in town.
	for _, other in hero.all() do
		if other ~= h and other.ready and nox.present(other.unit) then
			local x, y = nox.position(other.unit)
			if not town.contains(x, y) then
				nox.tell(h.unit, "Everyone must be in town to travel to another area.")
				return false, "party out"
			end
		end
	end
	return areas.travel(index, "waypoint")
end

-- The town's waypoints pentagram touched: the waypoints its toucher has found offered.
local function padTouched(unit: number)
	local h = hero.byUnit(unit)
	if not h or not areas then
		return
	end
	local act = areas.current().act
	local list = progress.waypoints(h.record, act)
	if #list == 0 then
		town.say(unit, "You've found no waypoint yet. Look for one out in the areas.")
		return
	end
	local o = offered[h.peer]
	if o and nox.frame() - o.frame < OFFER_AGAIN then
		return
	end
	local now = areas.current().index
	local answers = {}
	local shown = {}
	for _, index in list do
		if #answers < 9 then
			local info = areas.info(index)
			table.insert(answers, info.title .. (if index == now then " (here)" else ""))
			table.insert(shown, index)
		end
	end
	offered[h.peer] = { list = shown, frame = nox.frame() }
	nox.choice(h.peer, "Waypoints", answers)
end

-- onChoice: whether it was an answer to the waypoints' offer (and went where it said).
function waypoints.chosen(peer: number, index: number): boolean
	local o = offered[peer]
	if not o then
		return false
	end
	offered[peer] = nil
	local h = hero.byPeer(peer)
	local area = o.list[index]
	if h and area then
		waypoints.go(h, area)
	end
	return true
end

-- Something landed on the town's receiving pentagram: a hero came home by this map's waypoint
-- (found, if it wasn't); a monster that followed is sent back.
local function arrived(unit: number)
	local h = hero.byUnit(unit)
	if h then
		if h.ready then
			found(h)
		end
		return
	end
	if monsters.info(unit) and here then
		local x, y = nox.clearPointNear(here.arrive.x, here.arrive.y, 40)
		nox.setPosition(unit, x, y)
	end
end

-- A map's area entered (`spots`: the generator's; `info`: areas.info's; `areasApi`: areas.luau):
-- the town's pentagrams set to work, and this area's waypoint looked for.
function waypoints.place(spots: any, info: any, areasApi: any)
	areas = areasApi
	touch.adopt("TeleportPentagram", spots.town.waypoints.x, spots.town.waypoints.y, padTouched, 45)
	touch.adopt("InvisibleTeleportPentagram", spots.town.wayIn.x, spots.town.wayIn.y, arrived, 30, true)
	if spots.waypoint then
		here = { act = info.act, index = info.index, title = info.title, x = spots.waypoint.x, y = spots.waypoint.y, arrive = spots.wayArrive }
		beat = nox.every(numbers.waypoints.every, look)
	end
end

-- Every hero finds this map's waypoint (the game arrived beside it).
function waypoints.foundByAll()
	for _, h in hero.all() do
		found(h)
	end
end

-- This map's waypoint, for "dd": { index, x, y, arrive } or nil.
function waypoints.here(): any
	return here
end

-- A new map: the old one's waypoint is gone with it.
function waypoints.reset()
	here = nil
	table.clear(offered)
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

return waypoints
