-- The generator's level builder: from a level's parameters,
-- the layout (layout.luau), the traps and puzzles (features.luau), the walls round the floor, the
-- gates and floor (floors.luau), and the objects (starts, exit, gates' signs, the centre's things,
-- signs, scenery, items, monsters), each written through the map writer `w`, which keeps the
-- sections in the order shipped maps carry them. The design is here; the cell work in the native
-- blocks.
--
-- Its streams, each drawn in the order the steps run: the layout's, the features' (made at their
-- planning, drawn again at their placing), the floor's and the objects' (rng), as the C++
-- generator drew them.
--
-- The parameters (arena.hpp's ArenaParams, less the map's title, author, game types and player
-- counts, which buildArena writes itself): centerX, centerY, radius (a plain diamond room);
-- segments, corridor, endRadius (a winding level); wallSet, floorName, ambient (bare) or biome;
-- playerStarts; items, chests, monsters, centre; questExit; gates ({type, toBiome, sign}),
-- writeGates; signs; traps, puzzles, ambushes, doors, obelisks, depth; seed. What it returns:
-- where its things went ({end, centre, gates, features}).

local util = require("util")
local layout = require("layout")
local floors = require("floors")

local arena = {}

local ROW = util.ROW
local SIDES = util.SIDES
local SIDE_DX, SIDE_DY = util.SIDE_DX, util.SIDE_DY
local chebyshev = util.chebyshev
local centre = util.centre
local worldX, worldY = util.worldX, util.worldY
local abs, max = math.abs, math.max

local NE, SE, SW, NW = SIDES[1], SIDES[2], SIDES[3], SIDES[4]
local outOf, alongOf = floors.outOf, floors.alongOf

local function point(c: number)
	return { x = worldX(c), y = worldY(c) }
end

-- Where a gate (or the exit) stands before its wall: half a cell out from the wall's line,
-- south-east of a "/", south-west of a "\".
local function gatePoint(wc: number, shape: number)
	local x, y = util.xy(wc)
	return { x = (if shape == 0 then x + 1 else x) * 23, y = (y + 1) * 23 }
end

function arena.build(p, w)
	-- ---- The parameters ----------------------------------------------------------------------
	-- A plain room's centre and size are checked here; a winding layout places itself.
	local winding = p.segments > 0
	local r = p.radius
	if not winding then
		if (p.centerX + p.centerY) % 2 ~= 0 or r % 2 ~= 0 or r < 10 then
			gen.refuse("mapgen: centre must sit on the even checkerboard, the radius even and at least 10")
		end
		if p.centerX - r < 8 or p.centerX + r > 0xF6 or p.centerY - r < 8 or p.centerY + r > 0xF6 then
			gen.refuse("mapgen: the room must fit the map grid")
		end
	end
	local biome = nil
	if p.biome ~= "" then
		biome = floors.findBiome(p.biome)
		if not biome then
			gen.refuse("mapgen: no biome named " .. p.biome)
		end
	end
	-- A biome supplies the wall set, base floor and light; without one the params name them.
	local wallIdx = w:wallIndex(if biome then biome.wallSet else p.wallSet)
	local floorIdx = w:floorIndex(if biome then biome.floor else p.floorName)
	local ambientLight = if biome then biome.ambient else p.ambient

	-- ---- The layout, the features' cells taken out of it, the walls ------------------------------
	local lay = if winding
		then layout.winding({
			segments = p.segments,
			corridor = p.corridor,
			endRadius = p.endRadius,
			style = if biome then biome.style else "Natural",
			seed = p.seed,
		})
		else layout.diamond(p.centerX, p.centerY, r)
	local open = lay.open
	local start, finish = centre(lay.start), centre(lay.finish)
	-- (The features' planning and placing: winding levels only.)
	local features = if winding then require("features") else nil
	-- Everything a player must reach is reached from the start (side steps over the floor). (A
	-- plain room's floor is one diamond of the checkerboard's cells round its centre, all joined.)
	if winding then
		local reach = open:reach(start)
		if not reach:has(finish) then
			gen.refuse("mapgen: a layout's chamber can't be reached from its start")
		end
		for _, room in lay.sideRooms do
			if not reach:has(centre(room)) then
				gen.refuse("mapgen: a layout's chamber can't be reached from its start")
			end
		end
	end
	-- The vaults, doors and ambushes, their cells out of the floor walked (walls, the floor under
	-- them the base).
	local planned = { secret = {}, vaultRooms = {} }
	if features then
		planned = features.plan(lay, { winding = winding, puzzles = p.puzzles, doors = p.doors, ambushes = p.ambushes, seed = p.seed })
	end
	local walls = gen.wallsAround(open)
	-- The back walls the view looks at (not the secret walls: what hangs on a wall would hang in the
	-- air once it opens).
	local backs = gen.backWalls(walls, open, gen.cells(planned.secret))

	-- ---- The gates and the floor -----------------------------------------------------------------
	local floorPlan = gen.cellmap()
	local built = floors.build({
		layout = lay,
		walls = walls,
		backs = backs,
		biome = if biome then biome.name else "",
		gates = p.gates,
		base = floorIdx,
		wallSet = wallIdx,
		seed = p.seed,
		floor = floorPlan,
		layer = gen.cellmap(),
	}, w)
	local gateWalls, previews = built.gates, built.previews

	-- ---- Objects ---------------------------------------------------------------------------------
	local rng = gen.minstd(p.seed)
	local spots = { centre = {}, gates = {}, features = {} }
	-- The cells nothing goes on yet: nothing on a harming floor nor beside it (a monster set there
	-- would burn to death).
	local used = gen.cells()
	used:addAroundFloors(floorPlan, built.harming, 1)
	-- Open cells with open sides all round (one in from any wall), the ones free things go on.
	local roomy = gen.roomy(open)
	-- A free roomy cell at least `nearest` from the start (and no further than `farthest` from
	-- `near`, when given): marked used.
	local function freeCell(nearest: number, near: number?, farthest: number?)
		return gen.freeCell(rng, roomy, used, start, nearest, near, farthest or 0, 400)
	end
	-- A thing of `spec` at (x, y) ("Type@kinds": a reward marker of those kinds, always placed; the
	-- kinds' text read by the writer, hex, as the C++ generator read it).
	local function object(spec: string, x: number, y: number)
		local at = string.find(spec, "@", 1, true)
		if at then
			w:object(string.sub(spec, 1, at - 1), x, y, string.sub(spec, at + 1))
		else
			w:object(spec, x, y)
		end
	end
	local function objectAt(spec: string, c: number)
		object(spec, worldX(c), worldY(c))
	end
	-- The floor before a gate's (or the exit's) wall kept clear.
	local function keepFront(wc: number, shape: number)
		local away, along = outOf(shape), alongOf(shape)
		for d = -2, 2 do
			for depth = 1, 2 do
				used:add(wc + depth * away + d * along)
			end
		end
	end
	-- DefaultXfer things (trees, rocks, plants, furniture: no state of their own): the plain ones of
	-- `types`, in order.
	local function plainOnly(types)
		local out = {}
		for _, t in types do
			if w:isPlain(t) then
				out[#out + 1] = t
			end
		end
		return out
	end
	local function gateWallCells()
		local taken = {}
		for _, gw in gateWalls do
			taken[#taken + 1] = gw.wall
		end
		return taken
	end

	-- The players start round the start chamber's centre.
	do
		local sx, sy = lay.start.x, lay.start.y
		for i = 0, p.playerStarts - 1 do
			local side = if i % 2 == 0 then -2 else 2
			local c = util.cell(sx + side * (1 + i // 2), sy + side)
			used:add(c)
			w:bare("PlayerStart", worldX(c), worldY(c))
		end
	end
	spots["end"] = point(finish)

	-- The exit: GauntletExitA (EXIT, QUEST_EXIT) against a back "/" run near the end chamber's
	-- centre, three straight cells long: the arch lies along such a run, opening south-east, half a
	-- cell in front of the wall's line as G_Temple's B stands from its wall. A fork's gates go the
	-- same way, where their walls were found, but are only reported.
	local exitWall = nil
	if p.questExit then
		exitWall = backs:straightBack(walls, 0, finish, gateWallCells())
		if not exitWall then
			gen.refuse("mapgen: no straight back wall for the exit")
		end
		keepFront(exitWall, 0)
		local at = gatePoint(exitWall, 0)
		w:exit("GauntletExitA", at.x, at.y)
	end
	for _, gw in gateWalls do
		keepFront(gw.wall, gw.shape)
	end
	for _, pv in previews do -- (nothing over a gate's preview: it must show)
		for _, c in pv.cells do
			used:add(c)
		end
	end

	-- A gate's sign beside it (four or five cells along its wall, one cell out), on its outer side
	-- (a "/" gate's south-west, a "\" gate's south-east: away from the corner the two back walls
	-- meet at, so neither sign stands between two gates), and two of the larger pieces of where it
	-- leads on the other side.
	for i, gw in gateWalls do
		local g = p.gates[i]
		local outer = if gw.shape == 0 then -1 else 1
		local spot = { type = g.type, at = gatePoint(gw.wall, gw.shape), sign = { x = 0, y = 0 } }
		if p.writeGates then
			w:exit(g.type, spot.at.x, spot.at.y)
		end
		local signSide = 0
		local out = outOf(gw.shape)
		local runX, runY = SIDE_DX[gw.run], SIDE_DY[gw.run]
		local outX, outY = SIDE_DX[out], SIDE_DY[out]
		local signSpots = {}
		for _, depth in { 0, 1 } do -- (one cell out from the wall, else two)
			for _, step in { 4, 5, 6, 3, 7, 8, -4, -5, -6, -3, -7, -8 } do
				signSpots[#signSpots + 1] = gw.front + outer * step * gw.run + depth * out
			end
		end
		-- None of those free (a small end chamber, the other gate's strip): the nearest free floor
		-- about it, the outer side first, never in its mouth (two cells either way, three out).
		do
			local more = {}
			for dy = -10, 10 do
				for dx = -10, 10 do
					if (dx + dy) % 2 == 0 then
						local k = util.idiv(dx * runX + dy * runY, 2) -- (along the wall)
						local d = util.idiv(dx * outX + dy * outY, 2) -- (out from it)
						if not (d < 0 or (abs(k) <= 2 and d <= 3)) then
							more[#more + 1] = { (if k * outer < 0 then 100 else 0) + abs(abs(k) - 4) + 2 * d, #more + 1, gw.front + dx * ROW + dy }
						end
					end
				end
			end
			-- (a stable sort: by the score, then the order they came in)
			table.sort(more, function(a, b)
				if a[1] ~= b[1] then
					return a[1] < b[1]
				end
				return a[2] < b[2]
			end)
			for _, m in more do
				signSpots[#signSpots + 1] = m[3]
			end
		end
		local fx, fy = util.xy(gw.front)
		local signed = false
		for _, c in signSpots do
			-- (its side of the gate: along the wall, the run's way; straight out, the outer side)
			local cx, cy = util.xy(c)
			local k = util.idiv((cx - fx) * runX + (cy - fy) * runY, 2)
			if open:has(c) and not used:has(c) then
				used:add(c)
				spot.sign = point(c)
				signSide = if k ~= 0 then k else outer
				signed = true
				objectAt(g.sign, c)
				break
			end
		end
		if not signed then
			gen.refuse("mapgen: no room for a gate's sign")
		end
		local to = if g.toBiome ~= "" then floors.findBiome(g.toBiome) else nil
		if to then
			-- (its large pieces, else its medium ones)
			local pieces = plainOnly(to.large)
			if #pieces == 0 then
				pieces = plainOnly(to.medium)
			end
			local away = if signSide > 0 then -1 else 1
			local placed = 0
			local k = 4
			while k <= 8 and placed < 2 and #pieces > 0 do
				local c = gw.front + away * k * gw.run
				if open:has(c) and not used:has(c) then
					used:add(c)
					objectAt(pieces[rng:next() % #pieces + 1], c)
					placed += 1
					k += 1 -- (a cell between them)
				end
				k += 1
			end
		end
		spots.gates[#spots.gates + 1] = spot
	end

	-- The centre's things: the first on the end chamber's centre, the rest round it; then room round
	-- them (a guardian's floor to fight on, a shopkeeper or a fire in plain sight): no scenery within
	-- five cells.
	local centreClear = {}
	do
		local ex, ey = lay.finish.x, lay.finish.y
		for i, spec in p.centre do
			local c = nil
			if i == 1 and not used:has(finish) then
				c = finish
				used:add(c)
			else
				-- (rings two cells apart, so x + y stays even; a ring's cells in a random order)
				local ring = 2
				while ring <= max(2, lay.finish.radius - 1) and not c do
					local around = {}
					for dx = -ring, ring do
						local dy = ring - abs(dx)
						around[#around + 1] = util.cell(ex + dx, ey + dy)
						if dy ~= 0 then
							around[#around + 1] = util.cell(ex + dx, ey - dy)
						end
					end
					rng:shuffle(around)
					for _, t in around do
						if open:has(t) and not used:has(t) then
							c = t
							used:add(t)
							break
						end
					end
					ring += 2
				end
			end
			if not c then
				gen.refuse("mapgen: no room at the centre for " .. spec)
			end
			objectAt(spec, c)
			spots.centre[#spots.centre + 1] = point(c)
		end
		if #p.centre > 0 then
			for dy = -5, 5 do
				for dx = -5, 5 do
					if abs(dx) + abs(dy) <= 5 then
						centreClear[#centreClear + 1] = util.cell(ex + dx, ey + dy)
					end
				end
			end
		end
	end

	-- Traps and puzzles, before the scenery and the monsters take the room: their objects, groups,
	-- secret walls and the script that wires them.
	local monsters = table.clone(p.monsters)
	if features then
		-- The door a biome's built in (a pair of them across a wide corridor: one key opens both).
		local doorType = "WoodenDoor"
		if biome and (biome.name == "Crypt" or biome.name == "Dungeon") then
			doorType = "CryptDoor"
		elseif biome and (biome.name == "Castle" or biome.name == "Shop") then
			doorType = "WoodAndSteelDoor"
		end
		local placedFeatures = features.place({
			plan = planned,
			walls = walls,
			backs = backs,
			used = used,
			doorType = doorType,
			obelisks = p.obelisks,
			obeliskType = if biome and biome.style == "Built" then "Obelisk" else "ObeliskPrimitive",
			monsters = monsters,
			traps = p.traps,
			depth = p.depth,
		}, w)
		monsters = placedFeatures.monsters
		spots.features = placedFeatures.placed
	end

	-- Signs (the lobby's: a diamond room) along its south-west and south-east walls, three cells in,
	-- west to east.
	do
		local n = #p.signs
		for i, sign in p.signs do
			local step = max(1, (2 * (r - 4)) // max(1, n))
			local dx = -(r - 4) + (i - 1) * step + step // 2
			local dy = (r - 3) - abs(dx)
			local c = util.cell(p.centerX + dx, p.centerY + dy)
			used:add(c)
			w:sign(sign, worldX(c), worldY(c))
		end
	end

	-- The decorator. Kept clear: the path and a cell round it, the starts, the exit's front. Scenery
	-- is DefaultXfer things (plainOnly), so anything else in a table is passed over.
	if biome then
		local clear = used:clone()
		clear:addAll(centreClear)
		for _, list in { { lay.path }, lay.branches, lay.loops } do
			for _, cells in list do
				for _, c in cells do
					clear:addSquare(c, 1)
				end
			end
		end
		-- (Densities as for a room's whole area: an open cell stands for two.)
		local area = 2 * open:count()
		-- The cells less than 5 and 2 from a large piece (on both axes): how far apart the sizes keep
		-- from the large ones. (Apart 1 is the cell itself, which a piece has made used.)
		local nearLarge5, nearLarge2 = gen.cells(), gen.cells()
		-- One size: `count` pieces on free cells (`deepOnly`: two in from any wall; else one), three
		-- out from the players' start, each away from the large ones (`near`); each try a draw of a
		-- roomy cell, a piece a draw of its kind.
		local function scatter(types, count: number, near, isLarge: boolean, deepOnly: boolean)
			local usable = plainOnly(types)
			if #usable == 0 then
				return
			end
			local marks = if isLarge then { nearLarge5, 4, nearLarge2, 1 } else {}
			for _, put in gen.scatter(rng, roomy, count, #usable, 30, used, clear, near, if deepOnly then open else nil, start, 3, marks) do
				local c, kind = put[1], put[2]
				w:bare(usable[kind], worldX(c), worldY(c))
			end
		end
		scatter(biome.large, area // max(1, biome.largeEvery), nearLarge5, true, true)
		scatter(biome.medium, area // max(1, biome.mediumEvery), nearLarge2, false, true)
		scatter(biome.small, area // max(1, biome.smallEvery), nil, false, false)
		-- Along the back walls (those the view looks at; set by the front ones they'd stand before
		-- them, facing away): against every wallEvery-th, none near the exit. Each stands just off the
		-- wall's line toward the floor, as the shipped maps' do (G_CryptD's torches on their wall's
		-- line): a torch a fifth of a cell out (on the wall), anything bulkier half a cell (against it,
		-- not in it).
		local wallTypes = plainOnly(biome.wall)
		if #wallTypes > 0 then
			local fifth = gen.f32(0.2)
			local i = 1
			local n = backs:count()
			while i <= n do
				local wc, c = backs:at(i)
				local nearExit = exitWall ~= nil and chebyshev(c, exitWall) < 4
				if not nearExit and not used:has(c) and not clear:has(c) then
					local type = wallTypes[rng:next() % #wallTypes + 1]
					local out = if string.find(type, "Torch", 1, true) then fifth else 0.5
					local skip = false
					if string.sub(type, 1, 8) == "Bookcase" then
						-- A bookcase's face is its wall's: Bookcase1 along a "/" (its floor south-east),
						-- Bookcase2 along a "\" (south-west), a quarter of a cell out from the wall's line,
						-- its back to it (Con02A's, G_Castle's, Wiz01A's). (Bookcase3 / 4 are those walls'
						-- other faces.) Not on a corner.
						local shape = walls:get(wc)
						if shape == nil or shape > 1 then
							skip = true
						else
							type = if shape == 0 then "Bookcase1" else "Bookcase2"
							out = 0.25
						end
					end
					if not skip then
						local d = c - wc
						local reach = gen.f32(out * 23)
						local x = gen.f32(worldX(wc) + gen.f32(reach * SIDE_DX[d]))
						local y = gen.f32(worldY(wc) + gen.f32(reach * SIDE_DY[d]))
						w:bare(type, x, y)
						used:add(c)
					end
				end
				i += max(1, biome.wallEvery)
			end
		end
	end

	-- ---- The items, the chests and the monsters --------------------------------------------------
	-- Each of `specs` in its room of `rooms` while there are rooms, else (or with its room full)
	-- anywhere at least `elsewhere` from the start.
	local function placeEach(specs, rooms, elsewhere: number)
		for i, spec in specs do
			local c = nil
			if i <= #rooms then
				local room = rooms[i]
				c = freeCell(2, centre(room), max(1, room.radius - 1))
			end
			if not c then
				c = freeCell(elsewhere)
			end
			if c then
				objectAt(spec, c)
			end
		end
	end
	-- Items: one in each side room first (the explorer's find), the rest anywhere.
	placeEach(p.items, lay.sideRooms, 2)
	-- Chests: one at the back of each side room first (the vaults' first), then away from the start.
	local chestRooms = {}
	local vaulted = {}
	for _, room in planned.vaultRooms do
		chestRooms[#chestRooms + 1] = room
		vaulted[centre(room)] = true
	end
	for _, room in lay.sideRooms do
		if not vaulted[centre(room)] then
			chestRooms[#chestRooms + 1] = room
		end
	end
	placeEach(p.chests, chestRooms, if winding then 10 else max(4, r // 2))
	-- Monsters: a quarter guarding the side rooms, the rest spread along the route past the start (a
	-- winding level), or away from the centre (a room).
	local sideRooms, path = lay.sideRooms, lay.path
	for i, spec in monsters do
		local c = nil
		if #sideRooms > 0 and (i - 1) % 4 == 3 then
			local room = sideRooms[((i - 1) // 4) % #sideRooms + 1]
			c = freeCell(8, centre(room), room.radius)
		end
		if not c and winding and #path > 8 then
			local from = #path // 5
			local along = path[from + rng:next() % (#path - from) + 1]
			c = freeCell(8, along, 6)
		end
		if not c then
			c = freeCell(if winding then 8 else max(4, r // 2))
		end
		if c then
			objectAt(spec, c)
		end
	end

	w:ambient(ambientLight)
	return spots
end

return arena
