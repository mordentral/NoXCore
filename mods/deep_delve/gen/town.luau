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
-- written and returns where its stations stand.

local util = require("util")
local layout = require("layout")
local arch = require("arch")
local lights = require("lights")

local town = {}

local cell, worldX, worldY = util.cell, util.worldX, util.worldY

-- The town's diamond: its walls RADIUS from its centre (its floor within RADIUS - 2), and at least
-- CLEAR cells of rock between its walls and the area's open floor.
town.RADIUS = 12
local CLEAR = 6

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

-- The nearest open cell of the section to (x, y) that isn't used (rings outward); marked used.
local function freeNear(section, used, x: number, y: number): number
	for ring = 0, section.radius do
		for dx = -ring, ring do
			for dy = -ring, ring do
				if math.max(math.abs(dx), math.abs(dy)) == ring then
					local c = cell(x + dx, y + dy)
					if section.open:has(c) and not used:has(c) then
						used:add(c)
						return c
					end
				end
			end
		end
	end
	gen.refuse("deep_delve: the town is full")
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

-- The town's stations, starts, lights and scenery (site: the section; backs, walls: the map's),
-- written through `w`. Returns where its stations stand, for the server to work them:
-- { centre, healer, gate (with its arch's type), stash, vendor, portal, waypoints, wayIn }
-- (world points), and wayInExtent (the receiving pentagram's, for the area's waypoint to lead to).
function town.dress(section, site, w)
	local cx, cy = section.x, section.y
	local used = gen.cells()
	local spots = {}
	-- The stations: the gate out an arch in the town's back wall, the healer's fountain in the
	-- middle, the stash to the west, the vendor to the north.
	local middle = cell(cx, cy)
	for _, shape in { 1, 0 } do
		local at = arch.place(w, site, shape, middle, section.radius + 4, {}, used)
		if at then
			spots.gate = { x = at.x, y = at.y, type = if shape == 0 then "GauntletExitA" else "GauntletExitB" }
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
	-- The pentagrams. The portal's (east): back out through the portal a player opened. The
	-- waypoints' (south-east): to the waypoints found. Both lead nowhere by themselves (the server
	-- watches their touches and decides). And the waypoints' receiving end (south-west), where a
	-- waypoint out in the area sends its travellers home: an invisible pentagram, so arriving on it
	-- takes nobody anywhere.
	local function pentagram(thingType: string, dx: number, dy: number)
		local c = freeNear(section, used, cx + dx, cy + dy)
		used:addSquare(c, 1)
		local p = point(c)
		return p, w:thing(thingType, p.x, p.y, { transporter = { target = 0 } })
	end
	spots.portal = pentagram("TeleportPentagram", 6, -1)
	spots.waypoints = pentagram("TeleportPentagram", 5, 3)
	spots.wayIn, spots.wayInExtent = pentagram("InvisibleTeleportPentagram", -6, 3)
	lights.put(w, "portal", spots.portal.x, spots.portal.y)
	lights.put(w, "waypoint", spots.waypoints.x, spots.waypoints.y)
	lights.put(w, "fountain", spots.healer.x, spots.healer.y)
	-- Two braziers flanking the way to the vendor, their fire lit.
	for _, dx in { -3, 3 } do
		local c = freeNear(section, used, cx + dx, cy - 4)
		used:addSquare(c, 1)
		town.put(w, "Brazier", worldX(c), worldY(c))
		lights.put(w, "brazier", worldX(c), worldY(c))
	end
	-- Torches on its back walls (every third: the ones the view looks at), a fifth of a cell out
	-- from the wall as the generator's own stand.
	if w:isPlain("Torch") then
		local fifth = gen.f32(0.2)
		local k = 0
		for i = 1, site.backs:count() do
			local wc, c = site.backs:at(i)
			if section.open:has(c) then
				k += 1
				if k % 3 == 1 then
					local d = c - wc
					local reach = gen.f32(fifth * 23)
					local x = gen.f32(worldX(wc) + gen.f32(reach * util.SIDE_DX[d]))
					local y = gen.f32(worldY(wc) + gen.f32(reach * util.SIDE_DY[d]))
					w:bare("Torch", x, y)
					lights.put(w, "torch", x, y)
				end
			end
		end
	end
	-- A little scenery by the stash and the vendor.
	for _, piece in { { "Barrel", -7, 2 }, { "Barrel2", -7, -2 }, { "BarrelWithTools2", 2, -7 }, { "Barrel", -2, -7 } } do
		local c = freeNear(section, used, cx + piece[2], cy + piece[3])
		town.put(w, piece[1], worldX(c), worldY(c))
	end
	return spots
end

return town
