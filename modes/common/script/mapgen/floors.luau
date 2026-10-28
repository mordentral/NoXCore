-- A generated level's gates and floor: the design here, the
-- cell work in the native blocks (a floor plan is two gen.cellmaps: each cell's floor and its layer).
--
-- A fork's gates go in the end chamber's back walls (A along a "/", B along a "\"), each with the
-- floor of where it leads spilling through it. The floor: the base on every open cell and under
-- every wall (the floor shows past the front walls); with a biome, patches of its other floors,
-- rooms of their own and its path along the plotted route; the previews over them; then a lone
-- base cell among one overlay floor on three or four sides taken into it (the editor's painted
-- shapes have no such specks). Where two floors meet, the writer blends the lower layer with the
-- higher's edge: base 0, a room's own floor 1, a patch 2, the path 3, a gate's preview 4. A
-- harming floor (lava) keeps two cells off the route and the side paths, and out of the start and
-- end chambers: pools to walk round, never across the way.
--
-- The floor's draws come from its own stream, here and in the blocks and the writer's blending,
-- in the order the C++ generator made them.

local util = require("util")

local floors = {}

local ROW = util.ROW
local SIDES = util.SIDES
local centre = util.centre
local manhattan = util.manhattan
local max = math.max

local ROOM_LAYER, PATCH_LAYER, PATH_LAYER, PREVIEW_LAYER = 1, 2, 3, 4
local NE, SE, SW, NW = SIDES[1], SIDES[2], SIDES[3], SIDES[4]

-- The biomes by name, case aside. (Their data is loaded only when a biome is named: a plain room,
-- the lobbies', never needs it.)
local biomeNamed = nil
function floors.findBiome(name: string)
	if not biomeNamed then
		biomeNamed = {}
		for _, b in require("biomes").biomes do
			biomeNamed[string.lower(b.name)] = b
		end
	end
	return biomeNamed[string.lower(name)]
end

-- A straight back wall of `shape` (0 "/", 1 "\"): the way out from it to the open floor before it
-- (south-east of a "/", south-west of a "\"), and the way along it.
function floors.outOf(shape: number): number
	return if shape == 0 then SE else SW
end

function floors.alongOf(shape: number): number
	return if shape == 0 then NE else SE
end

-- The gates and the floor (site: layout, walls (a gen.cellmap of shapes), backs (gen.backs),
-- biome (a name, or ""), gates ({type, toBiome, sign}), base (the base floor's index), wallSet
-- (the walls' material), seed, floor and layer (gen.cellmaps, filled here)), written through `w`.
-- Returns {gates ({wall, shape, front, run}), previews ({cells, floor}), harming (the harming
-- floors laid)}.
function floors.build(site, w)
	local lay = site.layout
	local open = lay.open
	local walls = site.walls
	local finish = centre(lay.finish)
	local biome = if site.biome ~= "" then floors.findBiome(site.biome) else nil
	local harmsOf = {}
	local function harms(f: number): boolean
		local h = harmsOf[f]
		if h == nil then
			h = gen.harmingFloor(w:floorName(f))
			harmsOf[f] = h
		end
		return h
	end

	-- The fork's gates' walls, and the floor of where each leads spilling through it.
	local gateWalls, previews = {}, {}
	for _, g in site.gates do
		local shape = if g.type == "GauntletExitB" then 1 else 0
		local taken = {}
		for _, gw in gateWalls do
			taken[#taken + 1] = gw.wall
		end
		local wc = site.backs:straightBack(walls, shape, finish, taken)
		-- (In the end chamber. One squeezed small, or ragged, may have none: the level's drawn again.)
		if not wc or manhattan(wc, finish) > lay.finish.radius + 4 then
			gen.refuse("mapgen: no straight back wall for a gate in the end chamber (" .. g.type .. (if wc
				then ", nearest " .. tostring(manhattan(wc, finish)) .. " from its centre, radius " .. tostring(lay.finish.radius)
				else ", none at all") .. ")")
		end
		local out = floors.outOf(shape)
		local run = floors.alongOf(shape)
		gateWalls[#gateWalls + 1] = { wall = wc, shape = shape, front = wc + out, run = run }
		if g.toBiome ~= "" then
			local to = floors.findBiome(g.toBiome)
			if not to then
				gen.refuse("mapgen: no biome named " .. g.toBiome)
			end
			-- A strip before it: one to three cells out from its wall, three either side along it.
			local pv = { cells = {}, floor = w:floorIndex(to.floor) }
			for depth = 1, 3 do
				for along = -3, 3 do
					pv.cells[#pv.cells + 1] = wc + depth * out + along * run
				end
			end
			previews[#previews + 1] = pv
		end
	end

	-- The floor plan.
	local rng = gen.minstd(util.hash(site.seed, 2654435761, 17))
	local floor, layer = site.floor, site.layer
	local base = site.base
	floor:fill(open, base)
	floor:fill(walls, base)
	local used = { base } -- (each floor laid)
	-- The harming ones of floors laid, once each.
	local function harmingOf(list)
		local out, seen = {}, {}
		for _, f in list do
			if not seen[f] and harms(f) then
				seen[f] = true
				out[#out + 1] = f
			end
		end
		return out
	end
	local function paintPreviews()
		for _, pv in previews do
			for _, t in pv.cells do
				if open:has(t) then -- (by a back wall: nothing in front to show past)
					floor:set(t, pv.floor)
					layer:set(t, PREVIEW_LAYER)
				end
			end
			used[#used + 1] = pv.floor
		end
	end
	if biome then
		local patchFloors, patchHarms = {}, {}
		for _, name in biome.patches do
			local f = w:floorIndex(name)
			patchFloors[#patchFloors + 1] = f
			patchHarms[#patchHarms + 1] = harms(f)
			used[#used + 1] = f
		end
		if #patchFloors > 0 then
			-- (Only a harming floor's patches look at the cool ground.)
			local keepCool = gen.cells()
			local anyHarms = false
			for _, h in patchHarms do
				anyHarms = anyHarms or h
			end
			if anyHarms then
				for _, c in lay.path do
					keepCool:addSquare(c, 2)
				end
				for _, b in lay.branches do
					for _, c in b do
						keepCool:addSquare(c, 2)
					end
				end
				keepCool:addSquare(centre(lay.start), lay.start.radius + 3)
				keepCool:addSquare(finish, lay.finish.radius + 3)
			end
			-- Scatter about one patch per 35 open cells (at least two): square blobs of 2-4 cells' reach.
			local cells = open:list()
			floor:roughBlobs(layer, rng, cells, max(2, #cells // 35), patchFloors, patchHarms, open, keepCool, PATCH_LAYER)
		end
		-- Rooms of their own: two in five of the chambers and side rooms (not the start) floored wall
		-- to wall with one of the biome's other floors (never a harming one): a rug hall, a
		-- crypt-floored chamber, a mossy pocket.
		local roomFloors = {}
		for i, f in patchFloors do
			if not patchHarms[i] then
				roomFloors[#roomFloors + 1] = f
			end
		end
		local rooms = {}
		for i = 2, #lay.chambers do
			rooms[#rooms + 1] = lay.chambers[i]
		end
		for _, r in lay.sideRooms do
			rooms[#rooms + 1] = r
		end
		for _, room in rooms do
			if #roomFloors > 0 and rng:next() % 5 < 2 then
				local f = roomFloors[rng:next() % #roomFloors + 1]
				floor:paintWithin(layer, centre(room), room.radius, open, f, ROOM_LAYER)
			end
		end
		if biome.path ~= "" then
			-- (the side paths too, as worn as the route)
			local ways = table.clone(lay.path)
			for _, b in lay.branches do
				for _, c in b do
					ways[#ways + 1] = c
				end
			end
			local pathFloor = w:floorIndex(biome.path)
			floor:paintAround(layer, ways, open, pathFloor, PATH_LAYER)
			used[#used + 1] = pathFloor
		end
		paintPreviews()
		-- Two passes taking lone base cells into a floor that holds three or four of their sides,
		-- raising their layer to that floor's so the edges blend the same way round (a harming floor
		-- never spreads: a speck of ground in it stays).
		floor:absorb(layer, base, harmingOf(used), 2)
	else
		paintPreviews()
	end

	w:walls(walls, site.wallSet)
	-- (blended where the biome wants it: the edges drawn from the floor's stream)
	w:floors(floor, layer, if biome and biome.blend then w:edgeIndex(biome.edgeMask) else nil, rng)
	return { gates = gateWalls, previews = previews, harming = harmingOf(used) }
end

return floors
