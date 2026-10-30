-- An area's walls and floor: the biome's base floor everywhere, its patches and worn path on the
-- area's floor, the town's own floors on the town's (its look's: towns.luau), then both written
-- (w:walls, w:floors) once.
--
-- (The built-in floors module paints one layout; an area map holds two, the area and the town,
-- each with its own floor, so the painting is done here with the same native blocks.) The layers
-- decide which floor's edge blends over which: base 0, the town 1, a patch or the town's square
-- 2, the path or the town's paths 3 (the town's floors only ever meet each other: its walls stand
-- between them and the area's).

local util = require("util")

local ground = {}

local TOWN_LAYER, PATCH_LAYER, PATH_LAYER = 1, 2, 3

-- site: layout (the area's), town (its section), open (both floors' cells), walls (gen.cellmap),
-- biome (the built-in biome's table), townFloor (the town's floor, an index), townSquare ({ floor
-- index, radius } or nil), townPaths (a floor index or nil) and townWays (the cells its paths run
-- along), seed. Returns { floor (the gen.cellmap of floors), harming (the harming floors laid) }.
function ground.build(site, w)
	local lay, biome = site.layout, site.biome
	local rng = gen.minstd(util.hash(site.seed, 2654435761, 23))
	local floor, layer = gen.cellmap(), gen.cellmap()
	local base = w:floorIndex(biome.floor)
	floor:fill(site.open, base)
	floor:fill(site.walls, base)
	local laid = { base }
	local harming = {}

	-- Patches of the biome's other floors on the area's floor (never the town's). A harming one
	-- (lava) keeps two cells off the ways and out of the start and end chambers.
	local patchFloors, patchHarms = {}, {}
	for _, name in biome.patches or {} do
		local f = w:floorIndex(name)
		local h = gen.harmingFloor(name)
		table.insert(patchFloors, f)
		table.insert(patchHarms, h)
		table.insert(laid, f)
		if h then
			table.insert(harming, f)
		end
	end
	if #patchFloors > 0 then
		local keepCool = gen.cells()
		if #harming > 0 then
			for _, c in lay.path do
				keepCool:addSquare(c, 2)
			end
			for _, b in lay.branches do
				for _, c in b do
					keepCool:addSquare(c, 2)
				end
			end
			keepCool:addSquare(util.centre(lay.start), lay.start.radius + 3)
			keepCool:addSquare(util.centre(lay.finish), lay.finish.radius + 3)
		end
		local cells = lay.open:list()
		floor:roughBlobs(layer, rng, cells, math.max(2, #cells // 35), patchFloors, patchHarms, lay.open, keepCool, PATCH_LAYER)
	end
	-- The worn path along the route and the side ways.
	if biome.path and biome.path ~= "" then
		local ways = table.clone(lay.path)
		for _, b in lay.branches do
			for _, c in b do
				table.insert(ways, c)
			end
		end
		local pathFloor = w:floorIndex(biome.path)
		floor:paintAround(layer, ways, lay.open, pathFloor, PATH_LAYER)
		table.insert(laid, pathFloor)
	end
	-- The town's own floor, wall to wall.
	local town = site.town
	floor:paintWithin(layer, util.cell(town.x, town.y), town.radius, town.open, site.townFloor, TOWN_LAYER)
	table.insert(laid, site.townFloor)
	-- Its worn paths and the square round the fountain they lead from (a look that has them).
	if site.townPaths then
		floor:paintAround(layer, site.townWays, town.open, site.townPaths, PATH_LAYER)
		table.insert(laid, site.townPaths)
	end
	if site.townSquare then
		floor:paintWithin(layer, util.cell(town.x, town.y), site.townSquare.radius, town.open, site.townSquare.floor, PATCH_LAYER)
		table.insert(laid, site.townSquare.floor)
	end
	-- Lone base cells among another floor taken into it (no specks), never into a harming one.
	floor:absorb(layer, base, harming, 2)

	w:walls(site.walls, w:wallIndex(biome.wallSet))
	w:floors(floor, layer, if biome.blend then w:edgeIndex(biome.edgeMask) else nil, rng)
	return { floor = floor, harming = harming }
end

return ground
