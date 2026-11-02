-- The town section of a generated area (design 3.1): a walled diamond of its own, away from the
-- area's floor, reachable only by teleport. Every area map carries one, so a party is always in
-- one map whether its players are in town or in the field.
--
-- (The design's town is a hand-built prefab stamped into each map: engine ask A6. Until it's
-- built, the town is generated here: the same place in the map's life, its stations written in at
-- fixed spots, which the server then works.)
--
-- Two steps: `site` finds it room before the walls are fitted (its floor joins the map's open
-- floor), `dress` puts its stations, starts, pentagrams, lights and scenery in once the floor is
-- written and returns where its stations stand. Between them `ways` gives the cells its worn paths
-- run along (a look with paths), for the floor.
--
-- What it's dressed with is the act's look (towns.luau); the stations are the same in every look.

local util = require("util")
local layout = require("layout")
local arch = require("arch")
local lights = require("lights")
local towns = require("towns")

local town = {}

local cell, worldX, worldY = util.cell, util.worldX, util.worldY

-- The town's diamond: its walls RADIUS from its centre (its floor within RADIUS - 2), and at least
-- CLEAR cells of rock between its walls and the area's open floor.
town.RADIUS = 12
local CLEAR = 6

-- The portal's and the waypoints' pads, { dx, dy } cells from the middle (the free cell nearest
-- each): south-east of the fountain and south past the starts, both south of the way east to the
-- map table (towns.mapTable), so walking to it crosses neither.
local PADS = { portal = { 5, 3 }, waypoints = { 0, 8 } }

-- Whether no open cell lies within the square of `reach` round (cx, cy).
local function clearAround(open, cx: number, cy: number, reach: number): boolean
	for dx = -reach, reach do
		for dy = -reach, reach do
			if open:has(cell(cx + dx, cy + dy)) then
				return false
			end
		end
	end
	return true
end

-- A place for the town clear of the area's open floor: the map's corners first, then anywhere on
-- a coarse grid. Returns the section: { x, y, radius, open (its floor's cells, gen.cells) }.
function town.site(open)
	local r = town.RADIUS
	local reach = r + CLEAR
	-- (layout.diamond's rule: the centre on the even checkerboard, the room inside the map's grid)
	local lo, hi = 8 + r + 2, 0xF6 - r - 2
	local tries = { { lo, lo }, { hi, lo }, { lo, hi }, { hi, hi } }
	for x = lo, hi, 12 do
		for y = lo, hi, 12 do
			table.insert(tries, { x, y })
		end
	end
	for _, t in tries do
		local cx, cy = t[1], t[2]
		if (cx + cy) % 2 ~= 0 then
			cy -= 1
		end
		if clearAround(open, cx, cy, reach) then
			local lay = layout.diamond(cx, cy, r)
			return { x = cx, y = cy, radius = r, open = lay.open }
		end
	end
	gen.refuse("deep_delve: no room for the town")
	return nil
end

-- The nearest open cell of the section to (x, y) that isn't used (rings outward), and that `ok`
-- passes when given; marked used. `most`: no further than this many cells, else 0 (none).
local function freeNear(section, used, x: number, y: number, ok: ((number) -> boolean)?, most: number?): number
	for ring = 0, most or section.radius do
		for dx = -ring, ring do
			for dy = -ring, ring do
				if math.max(math.abs(dx), math.abs(dy)) == ring then
					local c = cell(x + dx, y + dy)
					if section.open:has(c) and not used:has(c) and (ok == nil or ok(c)) then
						used:add(c)
						return c
					end
				end
			end
		end
	end
	if most == nil then
		gen.refuse("deep_delve: the town is full")
	end
	return 0
end

local function point(c: number)
	return { x = worldX(c), y = worldY(c) }
end

-- A thing written into the map: a plain one (DefaultXfer) bare, any other with its own default
-- data. (Things that never move are in the map itself: a client draws them from its copy of the
-- map, never from the server's updates, so the stations must be written here, not made later.)
function town.put(w, thingType: string, x: number, y: number)
	if w:isPlain(thingType) then
		w:bare(thingType, x, y)
	else
		w:object(thingType, x, y)
	end
end

-- The wall cell the town's gate arch goes in (dress's choice: a "\" back wall nearest the middle,
-- else a "/" one, within the radius and four), or nil.
local function gateWall(section, site): number?
	local middle = cell(section.x, section.y)
	for _, shape in { 1, 0 } do
		local wc = site.backs:straightBack(site.walls, shape, middle, {})
		if wc and util.manhattan(wc, middle) <= section.radius + 4 then
			return wc
		end
	end
	return nil
end

-- The cells of a straight run from (x, y) toward (tx, ty), a cell a step on the longer axis, while
-- they're the town's floor.
local function run(section, out, x: number, y: number, tx: number, ty: number)
	local dx, dy = tx - x, ty - y
	local n = math.max(math.abs(dx), math.abs(dy))
	for i = 0, n do
		local c = cell(x + util.idiv(dx * i, math.max(1, n)), y + util.idiv(dy * i, math.max(1, n)))
		if not section.open:has(c) then
			break
		end
		table.insert(out, c)
	end
end

-- The cells of a run of side steps (the floor's own diagonals) from (x, y) to (tx, ty) (the same
-- checkerboard colour), zigzagging where one axis is already reached, while they're the town's
-- floor.
local function diagonal(section, out, x: number, y: number, tx: number, ty: number)
	local flip = 1
	for _ = 1, 64 do
		if x == tx and y == ty then
			break
		end
		local sx, sy = math.sign(tx - x), math.sign(ty - y)
		if sx == 0 then
			sx = flip
			flip = -flip
		elseif sy == 0 then
			sy = flip
			flip = -flip
		end
		x += sx
		y += sy
		local c = cell(x, y)
		if not section.open:has(c) then
			break
		end
		table.insert(out, c)
	end
end

-- The cells the look's worn paths run along (site: the map's backs and walls): from the middle west
-- to the stash, north to the vendor, east and south, to the gate's arch, and (along the floor's
-- diagonals) to the portal's and waypoints' pads and the map table. Nothing is drawn or used: the
-- same each time for the same section.
-- The ways' cells: the four from the middle, the one to the gate's wall cell `wc` (when any), and
-- the three to the pads and the table.
local function waysTo(section, wc: number?)
	local cx, cy = section.x, section.y
	local out = {}
	local reach = section.radius - 3
	run(section, out, cx, cy, cx - reach, cy)
	run(section, out, cx, cy, cx, cy - reach)
	run(section, out, cx, cy, cx + reach, cy)
	run(section, out, cx, cy, cx, cy + reach)
	if wc then
		local gx, gy = util.xy(wc)
		run(section, out, cx, cy, gx, gy)
	end
	for _, to in { PADS.portal, PADS.waypoints, towns.mapTable.at } do
		diagonal(section, out, cx, cy, cx + to[1], cy + to[2])
	end
	return out
end

function town.ways(section, site)
	return waysTo(section, gateWall(section, site))
end

-- The plain kinds of a list, in order.
local function plainOnly(w, list)
	local out = {}
	for _, t in list do
		if w:isPlain(t) then
			table.insert(out, t)
		end
	end
	return out
end

-- Where the map table stands (towns.mapTable): the free floor cell of the section against a back
-- wall nearest its place from the town's middle (the first of the map's back walls on a tie),
-- marked used with a cell round it; `out` of a cell from the wall's line, the desk whose back is to
-- that wall. Returns { x, y, type } and the wall's cell.
local function deskAt(section, site, used): (any, number)
	local mt = towns.mapTable
	local wx, wy = section.x + mt.at[1], section.y + mt.at[2]
	local best, bestWall, bestType, bestFar = 0, 0, "", math.huge
	for i = 1, site.backs:count() do
		local wc, c = site.backs:at(i)
		if section.open:has(c) and not used:has(c) then
			local d = c - wc
			-- (the wall north-east of its floor: "\"; north-west: "/")
			local kind = if util.SIDE_DY[d] == 1 and util.SIDE_DX[d] == -1 then mt.desk.ne
				elseif util.SIDE_DY[d] == 1 and util.SIDE_DX[d] == 1 then mt.desk.nw
				else nil
			if kind then
				local x, y = util.xy(c)
				local far = (x - wx) * (x - wx) + (y - wy) * (y - wy)
				if far < bestFar then
					best, bestWall, bestType, bestFar = c, wc, kind, far
				end
			end
		end
	end
	if best == 0 then
		gen.refuse("deep_delve: no back wall in town for the map table")
	end
	used:addSquare(best, 1)
	local d = best - bestWall
	local reach = gen.f32(gen.f32(mt.out) * 23)
	local x = gen.f32(worldX(bestWall) + gen.f32(reach * util.SIDE_DX[d]))
	local y = gen.f32(worldY(bestWall) + gen.f32(reach * util.SIDE_DY[d]))
	return { x = x, y = y, type = bestType }, bestWall
end

-- The town's stations, starts, lights and scenery (site: the section; backs, walls: the map's;
-- look: the act's, towns.luau), written through `w`. Returns where its stations stand, for the
-- server to work them: { centre, healer, gate (with its arch's type), stash, vendor, portal,
-- waypoints, wayIn, maps (the map table) } (world points), and
-- wayInExtent (the receiving pentagram's, for the area's waypoint to lead to).
function town.dress(section, site, w, look)
	local cx, cy = section.x, section.y
	local used = gen.cells()
	local spots = {}
	-- The stations: the gate out an arch in the town's back wall, the healer's fountain in the
	-- middle, the stash to the west, the vendor to the north.
	local middle = cell(cx, cy)
	local gateCell: number? = nil
	for _, shape in { 1, 0 } do
		local at, wc = arch.place(w, site, shape, middle, section.radius + 4, {}, used)
		if at then
			spots.gate = { x = at.x, y = at.y, type = if shape == 0 then "GauntletExitA" else "GauntletExitB" }
			gateCell = wc
			break
		end
	end
	if not spots.gate then
		gen.refuse("deep_delve: no wall in town for its gate")
	end
	spots.centre = point(middle)
	spots.healer = point(freeNear(section, used, cx, cy))
	spots.stash = point(freeNear(section, used, cx - 6, cy))
	spots.vendor = point(freeNear(section, used, cx, cy - 6))
	-- The stations themselves, room kept round each.
	for _, s in { { "Fountain", spots.healer }, { "Chest1", spots.stash }, { "Anvil1", spots.vendor } } do
		town.put(w, s[1], s[2].x, s[2].y)
		used:addSquare(cell(math.floor(s[2].x / 23), math.floor(s[2].y / 23)), 1)
	end
	-- The players' starts, south of the fountain: where a joining player comes in and the fallen
	-- come back.
	for i = 0, 3 do
		local c = freeNear(section, used, cx - 3 + 2 * i, cy + 5)
		w:bare("PlayerStart", worldX(c), worldY(c))
	end
	-- (the starts' row kept clear of the pentagrams below: a player brought in there mustn't stand
	-- on one)
	for i = -4, 4 do
		used:addSquare(cell(cx + i, cy + 5), 1)
	end
	-- The pentagrams. The portal's (south-east): back out through the portal a player opened. The
	-- waypoints' (south, past the starts): to the waypoints found. Both south of the way east from
	-- the fountain to the map table, so walking to it crosses neither (by decision, 2026-11-02), and
	-- their rings clear of each other and of the starts. Both lead nowhere by themselves (the server
	-- watches their touches and decides), so both are invisible pentagrams (touched as the visible
	-- kind is; the server draws a ring on the floor at each). And the waypoints' receiving end
	-- (south-west), where a waypoint out in the area sends its travellers home: an invisible
	-- pentagram, so arriving on it takes nobody anywhere.
	local function pentagram(thingType: string, dx: number, dy: number)
		local c = freeNear(section, used, cx + dx, cy + dy)
		used:addSquare(c, 1)
		local p = point(c)
		return p, w:thing(thingType, p.x, p.y, { transporter = { target = 0 } })
	end
	spots.portal = pentagram("InvisibleTeleportPentagram", PADS.portal[1], PADS.portal[2])
	spots.waypoints = pentagram("InvisibleTeleportPentagram", PADS.waypoints[1], PADS.waypoints[2])
	spots.wayIn, spots.wayInExtent = pentagram("InvisibleTeleportPentagram", -6, 3)
	lights.put(w, "portal", spots.portal.x, spots.portal.y)
	lights.put(w, "waypoint", spots.waypoints.x, spots.waypoints.y)
	lights.put(w, "fountain", spots.healer.x, spots.healer.y)
	-- The look's pair flanking the way to the vendor (braziers, torch poles), lit.
	local flank = look.flank
	local flanks = {}
	for _, dx in { -3, 3 } do
		local c = freeNear(section, used, cx + dx, cy - 4)
		used:addSquare(c, 1)
		town.put(w, flank.type, worldX(c), worldY(c))
		lights.put(w, flank.light, worldX(c), worldY(c))
		table.insert(flanks, point(c))
	end
	-- A little scenery by the stations.
	for _, piece in look.props do
		local c = freeNear(section, used, cx + piece[2], cy + piece[3])
		town.put(w, piece[1], worldX(c), worldY(c))
	end
	-- The map table (the expeditions'): a scholar's desk that never moves, lit gold (the server draws
	-- a gold ring on the floor round it, and touching it opens the window). At the same place in
	-- every look (towns.mapTable): beside the portal's pad, its back to the back wall there, and
	-- placed last, so nothing placed before it moves. Its spot carries its type, for the server.
	local deskWall
	spots.maps, deskWall = deskAt(section, site, used)
	town.put(w, spots.maps.type, spots.maps.x, spots.maps.y)
	lights.put(w, "maps", spots.maps.x, spots.maps.y)
	-- The look's trees round the fountain (the oasis's palms), after everything else: each off the
	-- worn ways (the paths between the stations and to the gate) and clear of the portal's and
	-- waypoints' pads.
	local grove = look.grove
	if grove then
		local ways = gen.cells()
		for _, c in waysTo(section, gateCell) do
			ways:add(c)
		end
		local far = grove.clear * 23
		local pads = { spots.portal, spots.waypoints }
		-- (what a tree's crown must leave in view: the stations, the pads, the gate and the lit pair
		-- by the vendor's way)
		local keep = { spots.healer, spots.stash, spots.vendor, spots.portal, spots.waypoints, spots.wayIn, spots.maps, spots.gate, flanks[1], flanks[2] }
		local across, up = grove.crown.across * 23, grove.crown.up * 23
		local function ok(c: number): boolean
			if ways:has(c) then
				return false
			end
			local x, y = worldX(c), worldY(c)
			for _, p in pads do
				if (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y) < far * far then
					return false
				end
			end
			-- (a crown stands over what is north of its foot)
			for _, p in keep do
				if p.y <= y and y - p.y <= up and math.abs(p.x - x) < across then
					return false
				end
			end
			return true
		end
		for i, at in grove.at do
			-- (the free cell nearest its place that passes, within `near` of it; none: no tree there)
			local c = freeNear(section, used, cx + at[1], cy + at[2], ok, grove.near)
			if c ~= 0 then
				used:addSquare(c, 1)
				town.put(w, grove.types[(i - 1) % #grove.types + 1], worldX(c), worldY(c))
			end
		end
	end
	-- The look's pieces on its back walls (every `every`-th: the ones the view looks at), `out` of
	-- a cell from the wall as the generator's own stand (a torch a fifth); none on the map table's
	-- wall cell, where the desk stands. (Last: they take no cell, so their place is the same
	-- wherever they're written.)
	local pieces = look.wallPieces
	local kinds = if pieces then plainOnly(w, pieces.types) else {}
	if pieces and #kinds > 0 then
		local out = gen.f32(pieces.out)
		local k = 0
		for i = 1, site.backs:count() do
			local wc, c = site.backs:at(i)
			if section.open:has(c) then
				k += 1
				if k % pieces.every == 1 % pieces.every and wc ~= deskWall then
					local kind = kinds[(k // pieces.every) % #kinds + 1]
					local d = c - wc
					local reach = gen.f32(out * 23)
					local x = gen.f32(worldX(wc) + gen.f32(reach * util.SIDE_DX[d]))
					local y = gen.f32(worldY(wc) + gen.f32(reach * util.SIDE_DY[d]))
					w:bare(kind, x, y)
					if pieces.light then
						lights.put(w, pieces.light, x, y)
					end
				end
			end
		end
	end
	return spots
end

return town
