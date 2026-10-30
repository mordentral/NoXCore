--!strict
-- Waypoints. Some areas have one (data/acts.luau): a lit teleport pentagram in a side room off the
-- route. Each character keeps the waypoints it has found (progress.luau, per tier and act).
--
--   * Found: by coming within numbers.waypoints.reach of it (looked for a few times a second, for
--     the heroes only), by arriving through it, or by arriving in town from it.
--   * Out in the area -> town: the waypoint is a real pentagram written into the map, leading to
--     the town's receiving pentagram; the game itself sends whoever stands on it (a pentagram
--     links places in one map, and the town is in every map).
--   * Town -> a waypoint: stepping on the town's waypoints pentagram (an invisible one under the
--     cyan ring) opens the waypoints
--     window on the toucher's UI (ui/main.luau): the areas that have a waypoint, by act (each
--     act open to its character), the ones it has found to click, the area being played marked.
--     This area's: a step across the map, at once, for that player alone (no vote). Another
--     area's, in this act or another: the whole game goes there (a map change, that area
--     generated into the other map tag), arriving beside its waypoint; with
--     anyone else in the game that's the party's vote (votes.luau: the ones in town asked too),
--     else (alone) it's at once. Walking away from the pentagram closes the window
--     (windows.luau's station), and so does a step or a vote started from it.
--   * The town's receiving pentagram watches what lands on it: a monster that followed a player
--     through is sent back to the waypoint it came by.
--
-- The town's waypoints pentagram is the mod's (watched, leading nowhere by itself) because only
-- the mod knows which waypoints a character has found.

local hero = require("hero")
local hud = require("hud")
local monsters = require("monsters")
local progress = require("progress")
local touch = require("touch")
local votes = require("votes")
local windows = require("windows")
local acts = require("data/acts")
local numbers = require("data/numbers")

local waypoints = {}

-- This map's waypoint: { act, index, title, x, y, arrive }, or nil.
local here: any = nil
local beat: number? = nil
-- areas.luau's own (handed in by place: it requires this module).
local areas: any = nil

-- The pentagram's touches come every PAD_EVERY frames while a player stands on it; a gap longer
-- than PAD_GAP is a new step onto it, which opens the window again (closed by its player, it
-- stays closed while the player still stands there).
local PAD_EVERY = 5
local PAD_GAP = 15

local function found(h: any)
	if not here or not progress.addWaypoint(h.record, here.act, here.index) then
		return
	end
	nox.effect(0x83, here.x, here.y) -- (MSG_FX_CYAN_SPARKS)
	if nox.present(h.unit) then
		nox.tell(h.unit, "Waypoint found: " .. here.title .. ". Its pentagram leads to town; the cyan ring in town leads back.")
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

-- A hero to the waypoint of area `index` of act `act` (one it has found; the act being played's
-- when `act` is nil): this area's at once (that hero alone); another's, in this act or another,
-- by a map change, the party's vote when anyone else is in the game. Whether it went or a vote
-- started ("vote"), else why.
function waypoints.go(h: any, index: number, act: number?): (boolean, string?)
	local current = areas.current()
	local place = areas.placeOf(act or current.act, index)
	if not place then
		return false, "no such area"
	end
	if place == current.place then
		local at = current.spots and current.spots.wayArrive
		if not at then
			return false, "no waypoint here"
		end
		local x, y = nox.clearPointNear(at.x, at.y, 30)
		nox.setPosition(h.unit, x, y)
		nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
		return true, nil
	end
	return votes.ask(h, place, "waypoint", "waypoint")
end

-- ---- The waypoints window

-- The window's value for a hero: { kind = "waypoints", act (the one being played), here (the area
-- being played), names (each act's name, by act), list = { { a = act, i = area, t = its title,
-- l = its level, f = found } }, the areas that have a waypoint, by act and area, of each act open
-- to the character (progress.isOpen) and the act being played }. (A few dozen bytes an area:
-- about 300 for an act; inside one value for the acts there are.)
local function valueFor(h: any): any
	local cur = areas and areas.current() or { act = 1, index = 0 }
	local list = {}
	local names = {}
	for a, act in ipairs(acts) do
		names[a] = act.name
		if a ~= cur.act and not progress.isOpen(h.record, a) then
			continue
		end
		for i, s in act.areas do
			local spec: any = s
			if spec.waypoint then
				table.insert(list, {
					a = a,
					i = i,
					t = (spec.title or spec.biome) .. " " .. i,
					l = act.base + i - 1,
					f = progress.hasWaypoint(h.record, a, i),
				})
			end
		end
	end
	return { kind = "waypoints", act = cur.act, here = cur.index, names = names, list = list }
end

-- The window's list to the hero's UI.
function waypoints.view(h: any)
	hud.send(h.peer, "waypoints", valueFor(h))
end

-- A message from the window: { op = "go", act, area } (a found waypoint clicked: of any act open
-- to the character; `act` nil: the act being played's). Whatever went (a step, or a vote
-- started) closes the window; a refusal is told in it.
function waypoints.message(h: any, v: any)
	if v.op ~= "go" or not windows.isOpen(h, "waypoints") or not areas then
		return
	end
	local area = tonumber(v.area)
	local act = tonumber(v.act) or areas.current().act
	local why: string? = nil
	if not area or not windows.near(h, "waypoints") then
		why = "Stand on the waypoints' ring."
	elseif not areas.placeOf(act, area) or (act ~= areas.current().act and not progress.isOpen(h.record, act))
		or not progress.hasWaypoint(h.record, act, area) then
		why = "You haven't found that waypoint yet."
	else
		local ok, refused = waypoints.go(h, area, act)
		if ok then
			windows.close(h, "waypoints", true)
			return
		end
		why = if refused == "a vote is under way" then "A vote to change area is under way: answer it first."
			elseif refused == "asked too soon" then "Your last vote failed a moment ago: ask again in a few seconds."
			else "Not now (" .. tostring(refused) .. ")."
	end
	windows.note(h, "waypoints", why :: string)
end

-- The town's waypoints pentagram touched: its window opened on the toucher's UI, at a new step
-- onto it (not again while it stands there after closing it).
local function padTouched(unit: number)
	local h = hero.byUnit(unit)
	if not h or not areas then
		return
	end
	local now = nox.frame()
	local last = h.padAt
	h.padAt = now
	if last and now - last <= PAD_GAP then
		return
	end
	windows.open(h, "waypoints")
end

windows.register("waypoints", { station = "waypoints", view = waypoints.view })

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
	touch.adopt("InvisibleTeleportPentagram", spots.town.waypoints.x, spots.town.waypoints.y, padTouched, PAD_EVERY)
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
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

return waypoints
