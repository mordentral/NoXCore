-- Deep Delve's area generator (nox.generate's module "area", fn "build"): one area map from the
-- server's parameters, run in the generator's own VM (the `gen` library and the map writer `w`;
-- no `nox`, no world).
--
-- An area is a winding route of rooms and corridors in its biome (the built-in layout module),
-- the town section away from it (town.luau), their walls and floors (ground.luau), the scenery
-- (scenery.luau), the town's stations and the arches between places (arch.luau: written into the
-- map, since things that never move are drawn from each client's copy of it), and the spots the
-- server needs: where players arrive, the way home, the exit, each monster pack, the end
-- chamber's champion, and the town's stations.
--
-- Two kinds of area take more:
--   * a waypoint area: a teleport pentagram in a side room off the route (else half way along
--     it), lit, that sends whoever stands on it to the town's receiving pentagram;
--   * the act's lair (its last area): no exit on; its end chamber is the boss's arena, braziers
--     and red light round its floor, the boss's spot in its middle.
--
-- params: biome (a built-in biome's name), corridor, segments, endRadius, packs (how many),
-- townFloor (a floor's name), waypoint (true: one here), lair (true: the act's lair), seed.
-- Returns the spots (world points), all plain numbers:
--   { arrive, back (with its arch's type), exit (too; none in a lair), guardian (none in a lair),
--     boss (a lair's), waypoint and wayArrive (a waypoint area's: the pentagram, and where
--     travellers to it arrive), packs = { {x, y}... },
--     town = { centre, healer, gate (with its type), stash, vendor, portal, waypoints, wayIn } }

local util = require("util")
local layout = require("layout")
local floors = require("floors")
local town = require("town")
local arch = require("arch")
local ground = require("ground")
local scenery = require("scenery")
local lights = require("lights")

local area = {}

local centre, worldX, worldY = util.centre, util.worldX, util.worldY

local function point(c: number)
	return { x = worldX(c), y = worldY(c) }
end

function area.build(p, w)
	local biome = floors.findBiome(p.biome)
	if not biome then
		gen.refuse("deep_delve: no biome named " .. tostring(p.biome))
	end

	-- ---- The layout: the area's route, and the town beside it. A layout must have a straight
	-- back wall in its end chamber for the exit (a lair needs none) and one in its start chamber
	-- for the way home; one that hasn't is laid again from the next seed, before anything is
	-- written (a few tries).
	local lay, start, finish, section, open, walls, backs
	local exitShapes = {}
	for attempt = 0, 7 do
		lay = layout.winding({
			segments = p.segments,
			corridor = p.corridor,
			endRadius = p.endRadius,
			-- (a lair's way in is a plain run of corridors: its one great chamber is the arena)
			chamberEvery = if p.lair then 99 else nil,
			style = biome.style,
			seed = p.seed + attempt * 7919,
		})
		start, finish = centre(lay.start), centre(lay.finish)
		if lay.open:reach(start):has(finish) then
			section = town.site(lay.open)
			open = lay.open:clone()
			open:addAll(section.open:list())
			walls = gen.wallsAround(open)
			backs = gen.backWalls(walls, open, gen.cells({}))
			local function wallFor(shape: number, near: number, reach: number, taken)
				local wc = backs:straightBack(walls, shape, near, taken)
				return if wc and util.manhattan(wc, near) <= reach then wc else nil
			end
			local taken = {}
			local ok = true
			if not p.lair then
				ok = false
				for _, shape in { 0, 1 } do
					local wc = wallFor(shape, finish, lay.finish.radius + 4, {})
					if wc then
						exitShapes = { shape }
						taken = { wc }
						ok = true
						break
					end
				end
			end
			if ok and (wallFor(1, start, lay.start.radius + 10, taken) or wallFor(0, start, lay.start.radius + 10, taken)) then
				break
			end
		end
		lay = nil
	end
	if not lay then
		gen.refuse("deep_delve: no layout with straight back walls for its ways")
	end

	-- ---- Walls and floors
	local plan = ground.build({
		layout = lay,
		town = section,
		open = open,
		walls = walls,
		biome = biome,
		townFloor = w:floorIndex(p.townFloor),
		seed = p.seed,
	}, w)

	-- ---- The area's spots
	local rng = gen.minstd(util.hash(p.seed, 2654435761, 29))
	local spots = {}
	-- Cells nothing goes on: nothing on a harming floor or beside it.
	local used = gen.cells()
	used:addAroundFloors(plan.floor, plan.harming, 1)
	local site = { backs = backs, walls = walls }
	-- The exit on: an arch ("/", else "\") in a back wall of the end chamber (a lair has none: the
	-- boss is the end).
	local taken = {}
	if not p.lair then
		local shape = exitShapes[1]
		local exitAt, exitWall = arch.place(w, site, shape, finish, lay.finish.radius + 4, {}, used)
		if not exitAt then
			gen.refuse("deep_delve: no straight back wall for the exit")
		end
		spots.exit = { x = exitAt.x, y = exitAt.y, type = if shape == 0 then "GauntletExitA" else "GauntletExitB" }
		taken = { exitWall }
	end
	-- The way home: an arch ("\", else "/") in a back wall of the start chamber; arriving players
	-- stand in the chamber's middle.
	spots.arrive = point(start)
	used:addSquare(start, 2)
	for _, shape in { 1, 0 } do
		local at = arch.place(w, site, shape, start, lay.start.radius + 10, taken, used)
		if at then
			spots.back = { x = at.x, y = at.y, type = if shape == 0 then "GauntletExitA" else "GauntletExitB" }
			break
		end
	end
	if not spots.back then
		gen.refuse("deep_delve: no straight back wall for the way home")
	end
	local roomy = gen.roomy(lay.open)
	local path, sideRooms = lay.path, lay.sideRooms
	if p.lair then
		-- The boss in the arena's middle (the open cell nearest it: some chambers have a pillar
		-- there), room kept round it; braziers at its four quarters, lit red, and a glow over it.
		local at = gen.freeCell(rng, roomy, used, start, 10, finish, 2, 200)
			or gen.freeCell(rng, roomy, used, start, 10, finish, lay.finish.radius, 400)
		if not at then
			gen.refuse("deep_delve: no floor for the boss")
		end
		spots.boss = point(at)
		used:addSquare(at, 3)
		local fx, fy = util.xy(finish)
		local k = math.max(3, lay.finish.radius - 3)
		for _, d in { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } do
			-- (the open cell nearest the quarter's point, from it toward the middle)
			for step = k, 2, -1 do
				local c = util.cell(fx + d[1] * step, fy + d[2] * step)
				if lay.open:has(c) and not used:has(c) then
					used:addSquare(c, 1)
					town.put(w, "Brazier", worldX(c), worldY(c))
					lights.put(w, "arena", worldX(c), worldY(c))
					break
				end
			end
		end
		lights.put(w, "arena", spots.boss.x, spots.boss.y)
	else
		-- The champion in the end chamber's middle.
		spots.guardian = point(finish)
		used:addSquare(finish, 2)
	end
	-- The waypoint: in the side room nearest the route's middle (a dead end off the way, found by
	-- looking round), else on the route half way along; where its travellers arrive a few cells
	-- off it (never on it: it would send them straight home).
	if p.waypoint then
		local mid = path[#path // 2 + 1]
		local best, bestD = nil, math.huge
		for _, room in sideRooms do
			local d = util.manhattan(centre(room), mid)
			if d < bestD then
				best, bestD = room, d
			end
		end
		-- (each place tried with room beside it for the arrivals: the side room, the route's
		-- middle, anywhere)
		local tries = {}
		if best then
			table.insert(tries, { centre(best), 2, 200 })
		end
		table.insert(tries, { mid, 6, 400 })
		table.insert(tries, { mid, 30, 400 })
		local wc, ac = nil, nil
		for _, t in tries do
			wc = gen.freeCell(rng, roomy, used, start, 10, t[1], t[2], t[3])
			if wc then
				local around = used:clone()
				around:addSquare(wc, 2)
				for r = 3, 6 do
					ac = gen.freeCell(rng, roomy, around, start, 0, wc, r, 200)
					if ac then
						break
					end
				end
				if ac then
					break
				end
			end
		end
		if not wc or not ac then
			gen.refuse("deep_delve: no room for the waypoint")
		end
		used:addSquare(wc, 2)
		used:addSquare(ac, 1)
		spots.waypoint = point(wc)
		spots.wayArrive = point(ac)
	end
	-- The packs: a quarter in the side rooms, the rest along the route past its first fifth; each
	-- at least ten cells from the start, kept apart.
	spots.packs = {}
	for i = 1, p.packs do
		local c = nil
		if #sideRooms > 0 and i % 4 == 0 then
			local room = sideRooms[(i // 4 - 1) % #sideRooms + 1]
			c = gen.freeCell(rng, roomy, used, start, 10, centre(room), room.radius, 400)
		end
		if not c and #path > 8 then
			local from = #path // 5
			local along = path[from + rng:next() % (#path - from) + 1]
			c = gen.freeCell(rng, roomy, used, start, 10, along, 6, 400)
		end
		if not c then
			c = gen.freeCell(rng, roomy, used, start, 10, nil, 0, 400)
		end
		if c then
			table.insert(spots.packs, point(c))
			used:addSquare(c, 4)
		end
	end

	-- ---- Scenery: off the ways (a cell round the route, its branches and loops)
	local clear = used:clone()
	for _, list in { { lay.path }, lay.branches, lay.loops } do
		for _, cells in list do
			for _, c in cells do
				clear:addSquare(c, 1)
			end
		end
	end
	scenery.place({ layout = lay, biome = biome, backs = backs, walls = walls, used = used, clear = clear, rng = rng }, w)

	-- ---- The town
	spots.town = town.dress(section, site, w)

	-- ---- The waypoint's pentagram, leading to the town's receiving one (written above), lit.
	if spots.waypoint then
		w:thing("TeleportPentagram", spots.waypoint.x, spots.waypoint.y, { transporter = { target = spots.town.wayInExtent } })
		lights.put(w, "waypoint", spots.waypoint.x, spots.waypoint.y)
	end

	w:ambient(biome.ambient)
	return spots
end

return area
