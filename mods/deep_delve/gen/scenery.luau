-- An area's scenery: the biome's large, medium and small pieces scattered over the area's floor,
-- and its wall pieces along the back walls, with the generator's own native scatter. The town
-- has its own (town.luau); nothing here goes on its floor.

local util = require("util")

local scenery = {}

local worldX, worldY = util.worldX, util.worldY

-- The plain kinds of a list (DefaultXfer things: trees, rocks, furniture), in order.
local function plainOnly(w, list)
	local out = {}
	for _, t in list do
		if w:isPlain(t) then
			table.insert(out, t)
		end
	end
	return out
end

-- site: layout, biome, backs, walls, town (its section), used (cells taken: kept free), clear
-- (cells kept clear of scenery: the ways), rng. Written through `w`.
function scenery.place(site, w)
	local lay, biome = site.layout, site.biome
	local open = lay.open
	local roomy = gen.roomy(open)
	local start = util.centre(lay.start)
	local area = 2 * open:count()
	local nearLarge5, nearLarge2 = gen.cells(), gen.cells()
	local function scatter(types, count: number, near, isLarge: boolean, deepOnly: boolean)
		local usable = plainOnly(w, types)
		if #usable == 0 or count <= 0 then
			return
		end
		local marks = if isLarge then { nearLarge5, 4, nearLarge2, 1 } else {}
		for _, put in gen.scatter(site.rng, roomy, count, #usable, 30, site.used, site.clear, near, if deepOnly then open else nil, start, 3, marks) do
			w:bare(usable[put[2]], worldX(put[1]), worldY(put[1]))
		end
	end
	scatter(biome.large, area // math.max(1, biome.largeEvery), nearLarge5, true, true)
	scatter(biome.medium, area // math.max(1, biome.mediumEvery), nearLarge2, false, true)
	scatter(biome.small, area // math.max(1, biome.smallEvery), nil, false, false)

	-- Wall pieces: against every wallEvery-th back wall of the area (not the town's), off the
	-- wall's line toward the floor: a torch a fifth of a cell, anything bulkier half a cell.
	local wallTypes = plainOnly(w, biome.wall)
	if #wallTypes == 0 then
		return
	end
	local fifth = gen.f32(0.2)
	local i = 1
	local n = site.backs:count()
	while i <= n do
		local wc, c = site.backs:at(i)
		if open:has(c) and not site.used:has(c) and not site.clear:has(c) then
			local kind = wallTypes[site.rng:next() % #wallTypes + 1]
			local out = if string.find(kind, "Torch", 1, true) then fifth else 0.5
			local skip = false
			if string.sub(kind, 1, 8) == "Bookcase" then
				-- (a bookcase faces its wall's way, and never stands on a corner)
				local shape = site.walls:get(wc)
				if shape == nil or shape > 1 then
					skip = true
				else
					kind = if shape == 0 then "Bookcase1" else "Bookcase2"
					out = 0.25
				end
			end
			if not skip then
				local d = c - wc
				local reach = gen.f32(out * 23)
				w:bare(kind, gen.f32(worldX(wc) + gen.f32(reach * util.SIDE_DX[d])), gen.f32(worldY(wc) + gen.f32(reach * util.SIDE_DY[d])))
				site.used:add(c)
			end
		end
		i += math.max(1, biome.wallEvery)
	end
end

return scenery
