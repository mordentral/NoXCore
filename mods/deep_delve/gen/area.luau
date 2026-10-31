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
--     it; an act's first area: by its start chamber), lit, that sends whoever stands on it to the
--     town's receiving pentagram;
--   * the act's lair (its last area): no exit on; its end chamber is the boss's arena in its look
--     (ARENAS below: braziers and red light round its floor; or a cold green light and a ring of
--     graves, the barrow-king's), the boss's spot in its middle, and (with an act after it) the
--     spot at the arena's back where the boss's death opens the gate on, its light written there.
--
-- An outdoor biome (grass, marsh: the built-in biomes' Open and Natural styles, their hedge,
-- root or field-stone walls and blended floors) is built the same way: the game's outdoor maps
-- are walled by their foliage and stones too.
--
-- params: biome (a built-in biome's name), corridor, segments, endRadius, packs (how many),
-- town (the act's town look: towns.luau's name), waypoint (true: one here), waypointAt ("start":
-- by the start chamber), lair (true: the act's lair), arena (a lair's look), gate (true: a lair's
-- gate spot), seed.
-- Returns the spots (world points), all plain numbers:
--   { arrive, back (with its arch's type), exit (too; none in a lair), guardian (none in a lair),
--     boss (a lair's), gate (a lair's with an act after it), graves (an arena look with graves:
--     each grave's point), waypoint and wayArrive (a waypoint
--     area's: the pentagram, and where travellers to it arrive), packs = { {x, y}... },
--     town = { centre, healer, gate (with its type), stash, vendor, portal, waypoints, wayIn,
--     look (its look's name) } }

local util = require("util")
local layout = require("layout")
local floors = require("floors")
local town = require("town")
local towns = require("towns")
local arch = require("arch")
local ground = require("ground")
local scenery = require("scenery")
local lights = require("lights")

local area = {}

local centre, worldX, worldY = util.centre, util.worldX, util.worldY

-- A lair's arena looks (params.arena, by name; none: "pyre"):
--   quarter  what stands at its four quarters, each with a light of kind `light` (gen/lights.luau);
--            a glow of `light` over the boss's spot too
--   graves   (when there are any) the graves set round the arena's edge, at most `count` of them,
--            one to each of eight sectors round its middle (and the outermost others, apart, up
--            to `least` in a small arena), each a type of the list in turn, kept off the way in
--            (the boss's dead climb out of them: spots.graves)
local ARENAS = {
	-- (act 1's: braziers lit red)
	pyre = { quarter = "Brazier", light = "arena" },
	-- (act 2's barrow-king's mound: braziers in a cold green, a ring of graves and stones)
	barrow = { quarter = "Brazier", light = "barrow", graves = { "Tombstone1", "Monument1", "Tombstone5" }, count = 8, least = 5 },
}

local function point(c: number)
	return { x = worldX(c), y = worldY(c) }
end

-- An open cell with open cells on its four sides, not taken.
local function roomyFree(open, used, c: number): boolean
	if not open:has(c) or used:has(c) then
		return false
	end
	for _, side in util.SIDES do
		if not open:has(c + side) then
			return false
		end
	end
	return true
end

-- A waypoint by the start chamber, and its travellers' arrival beside it: (waypoint cell, arrival
-- cell), both taken in `used`, or nil.
--
-- The cells near the start are looked at where they are, not drawn from every roomy cell of the
-- map (a big outdoor area's few cells by its start were drawn too seldom, and a draw five cells off
-- on both axes, ten steps away, was taken as "by the start" too). The cells from three to seven
-- steps from the start (|dx| + |dy|, at least three on one axis: clear of the arrivals' square),
-- nearest first, each with an arrival three to six cells from it (not within two of it); one of
-- the nearest four that has one is drawn.
local function byStart(rng, open, used, start: number): (number?, number?)
	local sx, sy = util.xy(start)
	local list = {}
	for dx = -7, 7 do
		for dy = -7, 7 do
			local d = math.abs(dx) + math.abs(dy)
			if d <= 7 and math.max(math.abs(dx), math.abs(dy)) >= 3 then
				local c = util.cell(sx + dx, sy + dy)
				if roomyFree(open, used, c) then
					table.insert(list, { c = c, d = d })
				end
			end
		end
	end
	-- (nearest first; then by cell, so the order never depends on the draw)
	table.sort(list, function(a, b)
		if a.d ~= b.d then
			return a.d < b.d
		end
		return a.c < b.c
	end)
	-- Each candidate's arrival: the nearest cell to it (3-6 off, by |dx| and |dy|), not by it.
	local function arrival(wc: number): number?
		local wx, wy = util.xy(wc)
		local around = used:clone()
		around:addSquare(wc, 2)
		for r = 3, 6 do
			for dx = -r, r do
				for dy = -r, r do
					if math.max(math.abs(dx), math.abs(dy)) == r then
						local c = util.cell(wx + dx, wy + dy)
						if roomyFree(open, around, c) then
							return c
						end
					end
				end
			end
		end
		return nil
	end
	local good = {}
	for _, e in list do
		local ac = arrival(e.c)
		if ac then
			table.insert(good, { c = e.c, a = ac })
			if #good >= 4 then
				break
			end
		end
	end
	if #good == 0 then
		return nil, nil
	end
	local pick = good[rng:next() % #good + 1]
	used:add(pick.c)
	used:add(pick.a)
	return pick.c, pick.a
end

function area.build(p, w)
	local biome = floors.findBiome(p.biome)
	if not biome then
		gen.refuse("deep_delve: no biome named " .. tostring(p.biome))
	end
	local look = towns.find(tostring(p.town))
	if not look then
		gen.refuse("deep_delve: no town look named " .. tostring(p.town))
	end
	local arena = ARENAS[p.arena or "pyre"]
	if not arena then
		gen.refuse("deep_delve: no arena look named " .. tostring(p.arena))
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

	-- ---- Walls and floors (the town's by its look)
	local plan = ground.build({
		layout = lay,
		town = section,
		open = open,
		walls = walls,
		biome = biome,
		townFloor = w:floorIndex(look.floor),
		townSquare = if look.square then { floor = w:floorIndex(look.square.floor), radius = look.square.radius } else nil,
		townPaths = if look.paths then w:floorIndex(look.paths) else nil,
		townWays = if look.paths then town.ways(section, { backs = backs, walls = walls }) else nil,
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
					town.put(w, arena.quarter, worldX(c), worldY(c))
					lights.put(w, arena.light, worldX(c), worldY(c))
					break
				end
			end
		end
		lights.put(w, arena.light, spots.boss.x, spots.boss.y)
		-- The gate's spot (a lair with an act after it): at the arena's back, away from its way
		-- in, clear of the boss. Nothing stands there until the boss dies (the server makes the
		-- gate then); only its light is written, which the server keeps off till then.
		if p.gate then
			local sx, sy = util.xy(start)
			local dx, dy = fx - sx, fy - sy
			local len = math.max(1, math.abs(dx), math.abs(dy))
			local reach = math.max(3, lay.finish.radius - 4)
			local back = util.cell(fx + util.idiv(dx * reach, len), fy + util.idiv(dy * reach, len))
			local gc = gen.freeCell(rng, roomy, used, start, 10, back, 3, 200)
				or gen.freeCell(rng, roomy, used, start, 10, finish, lay.finish.radius - 1, 400)
			if not gc then
				gen.refuse("deep_delve: no floor for the gate")
			end
			spots.gate = point(gc)
			used:addSquare(gc, 2)
			lights.put(w, "gate", spots.gate.x, spots.gate.y)
		end
		-- The graves round the arena (a look with graves): the floor within its reach (an open cell
		-- with open sides, from 4 cells off its middle to a cell past its nominal edge) parted into
		-- eight sectors round its middle (east, south-east, south...), and in each the outermost
		-- cell (the lowest cell number of equals) that isn't on the way in (a cell round the
		-- route) or by anything placed; written as the look's types in turn. No draw, so the rest of
		-- the map is as it would be without them.
		if arena.graves then
			local way = gen.cells()
			for _, c in path do
				way:addSquare(c, 1)
			end
			local reach = math.max(6, lay.finish.radius + 1)
			-- (a sector by the offset's signs, straight when one axis is over twice the other)
			local function sector(dx: number, dy: number): number
				local sx = if dx > 0 then 1 elseif dx < 0 then -1 else 0
				local sy = if dy > 0 then 1 elseif dy < 0 then -1 else 0
				if math.abs(dx) > 2 * math.abs(dy) then
					sy = 0
				elseif math.abs(dy) > 2 * math.abs(dx) then
					sx = 0
				end
				return (sx + 1) * 3 + (sy + 1)
			end
			local best: { [number]: { c: number, d: number } } = {}
			-- (every cell that may take one, the outermost first: for a small or lopsided arena
			-- whose sectors give fewer than `least`)
			local all: { { c: number, d: number } } = {}
			for dx = -reach, reach do
				for dy = -reach, reach do
					local dist = math.max(math.abs(dx), math.abs(dy))
					if dist >= 4 then
						local c = util.cell(fx + dx, fy + dy)
						if roomyFree(lay.open, used, c) and not way:has(c) then
							local k = sector(dx, dy)
							local b = best[k]
							if not b or dist > b.d or (dist == b.d and c < b.c) then
								best[k] = { c = c, d = dist }
							end
							table.insert(all, { c = c, d = dist })
						end
					end
				end
			end
			table.sort(all, function(a, b)
				if a.d ~= b.d then
					return a.d > b.d
				end
				return a.c < b.c
			end)
			spots.graves = {}
			local placed: { number } = {}
			local function grave(c: number)
				used:addSquare(c, 1)
				table.insert(placed, c)
				local at = point(c)
				town.put(w, arena.graves[(#placed - 1) % #arena.graves + 1], at.x, at.y)
				table.insert(spots.graves, at)
			end
			-- (in a fixed order round the arena)
			for _, k in { 7, 8, 5, 2, 1, 0, 3, 6 } do
				local b = best[k]
				if b and #placed < arena.count and roomyFree(lay.open, used, b.c) then
					grave(b.c)
				end
			end
			-- (too few: the outermost others, two cells from any placed)
			for _, e in all do
				if #placed >= arena.least then
					break
				end
				local apart = roomyFree(lay.open, used, e.c)
				for _, q in placed do
					apart = apart and util.chebyshev(q, e.c) >= 2
				end
				if apart then
					grave(e.c)
				end
			end
		end
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
		local wc, ac = nil, nil
		if p.waypointAt == "start" then
			-- (an act's first area: by the start chamber, from the cells near it, not a draw from
			-- the whole map: see byStart)
			wc, ac = byStart(rng, lay.open, used, start)
		end
		-- (each place tried with room beside it for the arrivals: the side room, the route's
		-- middle, anywhere)
		local tries = {}
		if best then
			table.insert(tries, { centre(best), 2, 200 })
		end
		table.insert(tries, { mid, 6, 400 })
		table.insert(tries, { mid, 30, 400 })
		for _, t in tries do
			if wc and ac then
				break
			end
			-- (at least 10 cells from the start)
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
	-- at least ten cells from the start, kept apart. `ramp` (0 or more) thins them out near the
	-- start: a route pack's place along the way is drawn with a density rising as (place) ^ ramp,
	-- 0 even all along. Each pack's spot says how far along the route it lies (`along`, 0 the
	-- start, 1 the end: the share of the route to the route cell nearest it), so the server can make
	-- the first ones smaller.
	local ramp = math.max(0, tonumber(p.ramp) or 0)
	local function alongOf(c: number): number
		local best, bestD = 1, math.huge
		for k, pc in path do
			local d = util.manhattan(pc, c)
			if d < bestD then
				best, bestD = k, d
			end
		end
		return (best - 1) / math.max(1, #path - 1)
	end
	spots.packs = {}
	for i = 1, p.packs do
		local c = nil
		if #sideRooms > 0 and i % 4 == 0 then
			local room = sideRooms[(i // 4 - 1) % #sideRooms + 1]
			c = gen.freeCell(rng, roomy, used, start, 10, centre(room), room.radius, 400)
		end
		if not c and #path > 8 then
			local from = #path // 5
			local span = #path - from
			local k = rng:next() % span
			if ramp > 0 then
				-- (the same draw, its share of the way bent toward the end)
				k = math.min(span - 1, math.floor(span * ((k + 0.5) / span) ^ (1 / (1 + ramp))))
			end
			c = gen.freeCell(rng, roomy, used, start, 10, path[from + k + 1], 6, 400)
		end
		if not c then
			c = gen.freeCell(rng, roomy, used, start, 10, nil, 0, 400)
		end
		if c then
			local at: any = point(c)
			at.along = alongOf(c)
			table.insert(spots.packs, at)
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
	spots.town = town.dress(section, site, w, look)
	spots.town.look = p.town

	-- ---- The waypoint's pentagram, leading to the town's receiving one (written above), lit.
	if spots.waypoint then
		w:thing("TeleportPentagram", spots.waypoint.x, spots.waypoint.y, { transporter = { target = spots.town.wayInExtent } })
		lights.put(w, "waypoint", spots.waypoint.x, spots.waypoint.y)
	end

	w:ambient(biome.ambient)
	return spots
end

return area
