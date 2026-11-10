--!strict
-- Waypoints. Some areas have one (data/acts.luau): a lit teleport pentagram in a side room off the
-- route. Each character keeps the waypoints it has found (progress.luau, per tier and act).
--
--   * Found: by coming within numbers.waypoints.reach of it (looked for a few times a second, for
--     the heroes only), by arriving through it, or by arriving in town from it.
--   * Out in the area -> town: the waypoint is a real pentagram written into the map, leading to
--     the town's receiving pentagram (a pentagram links places in one map, and the town is in
--     every map). It carries heroes alone (by decision 2026-11-10): its touches are the mod's, and
--     it works as the game's own pentagram does (see sendHome): a hero's step winds it up, and the
--     heroes on it at the wind-up's end go home with the game's flash at both ends, their creatures
--     with them; a monster or a creature on it stays where it is. It's a way (touch.luau): a hero
--     who arrived on it or beside it isn't taken back at once.
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
--
-- The town's waypoints pentagram is the mod's (watched, leading nowhere by itself) because only
-- the mod knows which waypoints a character has found.

local decals = require("decals")
local hero = require("hero")
local hud = require("hud")
local progress = require("progress")
local summons = require("summons")
local touch = require("touch")
local votes = require("votes")
local windows = require("windows")
local acts = require("data/acts")
local numbers = require("data/numbers")

local waypoints = {}

-- This map's waypoint: { act, index, title, x, y, arrive, home, id (its pentagram) }, or nil.
local here: any = nil
local beat: number? = nil
-- Its cycle under way (sendHome), or nil while it's idle: { up (winding up: true; down: false),
-- timer, decal (its second ring's id) }.
local wind: any = nil
-- areas.luau's own (handed in by place: it requires this module).
local areas: any = nil

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
		local fromX, fromY = nox.position(h.unit)
		nox.setPosition(h.unit, x, y)
		nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
		touch.arrived(h.unit)
		-- (its creatures with it)
		summons.bring(h.unit, fromX, fromY)
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

-- The town's waypoints pentagram stepped on: its window opened on the toucher's UI (heard only at
-- a new step onto it, so not again while it stands there after closing it; a way, so not straight
-- after arriving: touch.luau).
local function padTouched(unit: number)
	local h = hero.byUnit(unit)
	if not h or not areas then
		return
	end
	windows.open(h, "waypoints")
end

windows.register("waypoints", { station = "waypoints", view = waypoints.view })

-- ---- This map's waypoint (its touches the mod's: see waypoints.place), worked as the game's own
-- pentagram works (by decision 2026-11-10: its wind-up waited):
--   * a hero's touch while it's idle winds it up for numbers.waypoints.windUp frames (the game's
--     pentagram counts its frames 1-8, one a frame; its picture turns through them as it does,
--     which no mod can set, so a second ring turns fast over its own the while: decals.luau);
--   * at the wind-up's end the heroes standing on it then go home (sendOne), whoever touched it
--     first or not; one who stepped off in the meantime stays, and nothing is sent (no flash, no
--     sound) when nobody's on it, as the game's pentagram has it;
--   * it then winds down for numbers.waypoints.windDown frames (the slower ring), taking nobody:
--     a touch then does nothing, as the game's. Idle again, a hero still on it winds it up again.
--   * a hero held by the arrival's grace (touch.luau: arrived on it or beside it, not stepped off
--     and on again since) neither winds it up nor goes at its end.

-- Whether a unit stands on the waypoint as the game's pentagram sends what's on it: its collision
-- box over the square of the pentagram's radius about it.
local function onIt(unit: number): boolean
	if not here or not here.id or not nox.present(here.id) or not nox.present(unit) then
		return false
	end
	local px, py, pr = nox.position(here.id)
	local x, y, r = nox.position(unit)
	return math.abs(x - px) < pr + r and math.abs(y - py) < pr + r
end

-- A hero home to the town's receiving pentagram as the game's pentagram sends one: its flash and
-- sound where it stood and where it lands, its creatures with it; arriving (touch.luau); the
-- waypoint found, if it wasn't.
local function sendOne(h: any)
	local unit = h.unit
	local sound = numbers.waypoints.sound
	local x, y = nox.position(unit)
	nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
	nox.sound(unit, sound)
	nox.setPosition(unit, here.home.x, here.home.y)
	nox.effect(0x89, here.home.x, here.home.y)
	nox.sound(unit, sound)
	touch.arrived(unit)
	summons.bring(unit, x, y, sound)
	if h.ready then
		found(h)
	end
end

local function idle()
	wind = nil
end

-- Its second ring of `kind` ("windUp", "windDown") in place of the last.
local function ring(kind: string?)
	if wind and wind.decal then
		decals.remove(wind.decal)
		wind.decal = nil
	end
	if wind and kind and here then
		wind.decal = decals.put(kind, here.x, here.y)
	end
end

local function windEnds()
	if not wind or not here then
		return
	end
	wind.up = false
	ring("windDown")
	wind.timer = nox.after(numbers.waypoints.windDown, function()
		ring(nil)
		idle()
	end)
	for _, h in hero.all() do
		if nox.living(h.unit) and onIt(h.unit) and not touch.holds(here.id, h.unit) then
			sendOne(h)
		end
	end
end

-- A hero's touch (one the arrival's grace let through: touch.luau): wound up, if it's idle.
local function sendHome(unit: number)
	if wind or not here or not hero.byUnit(unit) or not nox.living(unit) then
		return
	end
	wind = { up = true, timer = 0, decal = nil }
	ring("windUp")
	wind.timer = nox.after(numbers.waypoints.windUp, windEnds)
end

-- A map's area entered (`spots`: the generator's; `info`: areas.info's; `areasApi`: areas.luau):
-- the town's pentagram set to work, and this area's waypoint looked for.
--
-- The area's waypoint carries heroes alone (by decision 2026-11-10): no monster and no risen zombie
-- goes through it to town, and a hero's creatures go only with their hero (summons.bring). It is
-- the game's own visible pentagram, kept as it is
-- written into the map (lit, leading to the town's receiving pentagram), but its touches are the
-- mod's (nox.watchCollide, touch.adopt): the game never sends anything by it, and sendHome winds it
-- up for a hero who steps on it. It's a way (touch.luau's arrival grace), every touch heard. The
-- town's receiving pentagram is an invisible one leading nowhere: walked over, it takes nobody
-- anywhere, so it needs no watching.
function waypoints.place(spots: any, info: any, areasApi: any)
	areas = areasApi
	touch.adopt("InvisibleTeleportPentagram", spots.town.waypoints.x, spots.town.waypoints.y, padTouched, nil, nil, "way step")
	if spots.waypoint then
		here = {
			act = info.act,
			index = info.index,
			title = info.title,
			x = spots.waypoint.x,
			y = spots.waypoint.y,
			arrive = spots.wayArrive,
			home = spots.town.wayIn,
		}
		here.id = touch.adopt("TeleportPentagram", spots.waypoint.x, spots.waypoint.y, sendHome, 1, nil, "way")
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

-- Its cycle now, for "dd waypoint wind": "idle", "up" or "down".
function waypoints.wind(): string
	return if not wind then "idle" elseif wind.up then "up" else "down"
end

-- A new map: the old one's waypoint is gone with it, and its cycle (its ring went with the map's
-- decals).
function waypoints.reset()
	here = nil
	if wind then
		nox.cancelTimer(wind.timer)
		wind = nil
	end
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

return waypoints
