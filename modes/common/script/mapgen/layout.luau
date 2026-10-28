-- A generated level's layout: where its floor is open. The
-- design is here, the cell work in the native blocks (gen.plot, gen.cells, gen.room).
--
-- A long level is plotted as a path first: diagonal segments winding across the map, mostly
-- eastward but doubling back now and then (Nox's straight walls run along the diagonals; a run
-- along an axis is a zigzag), each with its own width, chambers at its start, its end and turns
-- along the way (now and then an arena). Side branches leave the route at right angles, each
-- ending in a dead-end room (the explorer's loot). No part of the route or a branch comes near
-- another but where they join, so corridors never merge into one another. Loops are second ways
-- round. The corridors and chambers are the open floor; the writer fits walls round it.
--
-- Its draws come from the layout's own stream, here and in the blocks, in the order the C++
-- generator made them, so a seed lays out the level it always did.

local util = require("util")

local layout = {}

local ROW = 4096
local OFF = 2048
local max, min = math.max, math.min

-- Room shapes (the native blocks' numbers).
local DIAMOND, HALL, CROSS, RING, PILLARS, CAVERN = 0, 1, 2, 3, 4, 5
layout.DIAMOND, layout.HALL, layout.CROSS, layout.RING, layout.PILLARS, layout.CAVERN = DIAMOND, HALL, CROSS, RING, PILLARS, CAVERN

-- The map's usable wall grid: room left round the edge for the outermost walls.
local LOW, HIGH = 10, 244
-- Open floor of two separate corridors or rooms keeps this much more apart than their two radii:
-- both their walls (two cells each) and a little rock between.
local GAP = 6
-- The route's last blobs a new segment may come near (it starts from them).
local JOINT = 10
-- The sides, in the order a segment's ways are tried (the order its random pick indexes).
local DIAGONALS = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } }

local function key(x: number, y: number): number
	return x * ROW + y + OFF
end

local function inBounds(x: number, y: number, margin: number): boolean
	return LOW + margin <= x and x <= HIGH - margin and LOW + margin <= y and y <= HIGH - margin
end

local function between(rng, lo: number, hi: number): number
	return lo + rng:next() % max(1, hi - lo + 1)
end

-- A shape's odds: taken when the roll (0-99) is below its bound (and no earlier entry's).
local BUILT_ARENA = { { 40, PILLARS }, { 75, RING }, { 100, HALL } }
local BUILT_ROOM = { { 35, HALL }, { 55, PILLARS }, { 70, CROSS }, { 85, RING }, { 100, DIAMOND } }
local NATURAL = { { 60, CAVERN }, { 85, DIAMOND }, { 100, HALL } }
local OPEN = { { 50, CAVERN }, { 80, DIAMOND }, { 100, HALL } }

local function shapeFor(roll: number, odds): number
	for _, o in odds do
		if roll < o[1] then
			return o[2]
		end
	end
	return odds[#odds][2]
end

-- A shape for a room of `radius` in `style` (an arena's, or the end's that must have straight
-- walls for an exit); too small for one, a plainer one.
local function pickShape(style: string, radius: number, arena: boolean, isEnd: boolean, rng): number
	local roll = rng:next() % 100
	local shape
	if style == "Built" then
		shape = shapeFor(roll, if arena then BUILT_ARENA else BUILT_ROOM)
	elseif style == "Natural" then
		shape = shapeFor(roll, NATURAL)
	else
		shape = shapeFor(roll, OPEN)
	end
	if isEnd and shape ~= HALL and shape ~= PILLARS then
		-- (straight back walls)
		shape = if style == "Built" and rng:next() % 2 == 0 then HALL else DIAMOND
	end
	if (shape == RING and radius < 10) or (shape == PILLARS and radius < 8) then
		shape = HALL
	end
	if shape == CROSS and radius < 6 then
		shape = DIAMOND
	end
	return shape
end

local function room(x: number, y: number, radius: number)
	return { x = x, y = y, radius = radius, shape = DIAMOND, a = 0, b = 0, seed = 0 }
end

-- One diamond room (the lobby, a plain arena): open within radius - 2 of the centre, its walls at
-- radius.
function layout.diamond(cx: number, cy: number, radius: number)
	local plot = gen.plot()
	local only = room(cx, cy, radius - 2)
	plot:add(only)
	return {
		open = plot:carve({}, 0, key(cx, cy), 0, false), -- (one diamond: no holes, no pinholes)
		path = { key(cx, cy) },
		branches = {},
		loops = {},
		start = only,
		finish = only,
		chambers = { only },
		sideRooms = {},
	}
end

-- A long level across the map, with side branches (as many segments as fit). The style sets the
-- rooms' shapes and sizes, the corridors' widths and lengths:
--   Built: halls, pillared halls, crosses, rings and diamonds (7-11, one in four an arena of
--          12-15); corridors 2 wide (now and then 3), 9-16 long.
--   Natural: caverns mostly, some diamonds and halls (8-12, arenas 13-16); corridors 2 to the
--          widest, 10-18 long.
--   Open: caverns and diamonds, some halls (11-15, arenas 16-20); trails 2 to the widest, 12-22
--          long.
-- A room may overlap the route leading in (and the corridor leaving it) by half its radius + 2
-- blobs more than the usual joint: big rooms fit. Side rooms (2 + segments / 2 of them) are cut
-- the same way, 4-8 (Open 5-9). The last segment heads north-east or north-west if it can, so the
-- end chamber's back walls are whole for its exit or gates.
-- p: segments, corridor (the widest a corridor's half width gets), chamberEvery (a chamber every
-- this many turns), endRadius (the end chamber at most this big), style, seed.
function layout.winding(p)
	local rng = gen.mt19937(util.hash(p.seed, 2654435761, 5))
	local chamberEvery = p.chamberEvery or 2
	local path, branches, loops, chambers, sideRooms = {}, {}, {}, {}, {}
	-- The open floor being plotted (gen.plot: blobs in the order laid; tag 1 the route's); the
	-- route's cells as points, for the branches' roots and the loops' ends.
	local plot = gen.plot()
	local route = gen.plot()
	local px, py = {}, {}
	local START_RADIUS = 6
	local ROUTE_MARGIN = 12 -- (room for any chamber round a route cell)
	local ax, ay = LOW + ROUTE_MARGIN + 2, 128 -- (x + y even)
	local start = room(ax, ay, START_RADIUS)
	chambers[1] = start
	plot:add(start, 1)
	local function onRoute(x: number, y: number)
		path[#path + 1] = key(x, y)
		px[#px + 1] = x
		py[#py + 1] = y
		route:point(x, y)
	end
	onRoute(ax, ay)
	local lastX, lastY = nil, nil -- the last segment's way
	-- The style's measures: corridors' widths and lengths, rooms' sizes (arenas bigger).
	local style = p.style
	local isOpen = style == "Open"
	local built = style == "Built"
	local narrowest = 2
	local widest = max(narrowest, p.corridor)
	local shortest = if built then 9 elseif isOpen then 12 else 10
	local longest = if built then 16 elseif isOpen then 22 else 18
	local roomLo = if built then 7 elseif isOpen then 11 else 8
	local roomHi = if built then 11 elseif isOpen then 15 else 12
	local arenaLo = if built then 12 elseif isOpen then 16 else 13
	local arenaHi = if built then 15 elseif isOpen then 20 else 16
	-- A room may meet the route that leads in (and the corridor that leaves it) closer than other
	-- blobs, by a margin that grows with it: those cells lie inside it or at its door.
	local function since(span: number): number
		local n = plot:count()
		return if n > span then n - span else 0
	end
	local slack = 0 -- (after a room: its margin, for the next corridor)
	local function corridorWidth(): number
		if built then
			return if rng:next() % 4 == 0 then min(3, widest) else 2 -- (narrow passages between the halls)
		end
		return between(rng, narrowest, widest)
	end
	local segments = p.segments
	for s = 0, segments - 1 do
		-- A way for it: east twice as likely as west, never straight back; length and width of its
		-- own; it must stay on the map and clear of the route behind it.
		local laid = false
		-- (The last goes north-east or north-west if it can: it comes into the end chamber through
		-- a front wall, leaving its back walls whole, where an exit or a fork's gates go.)
		local lastOne = s + 1 == segments
		local attempt = 0
		while attempt < 24 and not laid do
			local ways = {}
			for _, d in DIAGONALS do
				local skip = (lastX ~= nil and d[1] == -lastX and d[2] == -lastY)
					-- The last segment tries only north-going ways for its first 16 attempts.
					or (lastOne and attempt < 16 and d[2] ~= -1)
				if not skip then
					ways[#ways + 1] = d
					if d[1] > 0 then
						ways[#ways + 1] = d
					end
				end
			end
			local way = ways[rng:next() % #ways + 1]
			local length = between(rng, shortest, longest)
			local width = corridorWidth()
			-- The new run may come near only the route's last JOINT blobs (plus a room's margin just
			-- after one), where it starts from.
			local cells = plot:run(ax, ay, way[1], way[2], length, ROUTE_MARGIN, width, GAP, since(JOINT + slack), 1)
			if cells then
				for _, c in cells do
					local x, y = util.xy(c)
					onRoute(x, y)
				end
				ax, ay = px[#px], py[#py]
				lastX, lastY = way[1], way[2]
				laid = true
				slack = 0
			end
			attempt += 1
		end
		if not laid then
			break -- (boxed in: the route ends here)
		end
		-- A chamber at this turn now and then, shaped by the style; one in four an arena.
		if (s + 1) % max(1, chamberEvery) == 0 and s + 1 < segments then
			local arena = rng:next() % 4 == 0
			local radius = if arena then between(rng, arenaLo, arenaHi) else between(rng, roomLo, roomHi)
			-- Shrink the room a step at a time until it fits.
			for r = radius, 4, -1 do
				local c = gen.room(rng, ax, ay, r, pickShape(style, r, arena, false, rng))
				local margin = c.radius // 2 + 2
				if inBounds(ax, ay, c.radius + 2) and plot:clear(ax, ay, c.radius, GAP, since(JOINT + margin)) then
					chambers[#chambers + 1] = c
					plot:add(c)
					slack = margin + 1
					break
				end
			end
		end
	end
	-- The end chamber, as big as fits (endRadius at the most; Open, two more), with straight back
	-- walls for its exit.
	local finish = room(ax, ay, 3)
	for r = max(3, p.endRadius + (if isOpen then 2 else 0)), 3, -1 do
		local c = gen.room(rng, ax, ay, r, pickShape(style, r, false, true, rng))
		if inBounds(ax, ay, c.radius + 2) and plot:clear(ax, ay, c.radius, GAP, since(JOINT + c.radius // 2 + 2)) then
			finish = c
			break
		end
	end
	chambers[#chambers + 1] = finish
	plot:add(finish)

	-- Side branches: off the route at right angles (away from its start and end), two wide, each
	-- ending in a room. The branch may meet the route only where it leaves it: route blobs (tag 1)
	-- within 12 of its root are passed over, and its first five cells.
	local wanted = 2 + segments // 2
	local n = #path
	local b, attempts = 0, 0
	while b < wanted and attempts < 240 and n > 20 do
		attempts += 1
		-- A route cell between the first sixth and the last sixth of the route.
		local i = n // 6 + rng:next() % (n * 2 // 3) + 1
		local rx, ry = px[i], py[i]
		-- The branch leaves at a right angle to the route's direction there, to one side or the other.
		local alongX, alongY = px[i + 1] - rx, py[i + 1] - ry
		local wx, wy
		if rng:next() % 2 == 0 then
			wx, wy = alongX, -alongY
		else
			wx, wy = -alongX, alongY
		end
		local length = if isOpen then between(rng, 9, 16) else between(rng, 7, 14)
		local sideRadius = if isOpen then between(rng, 5, 9) else between(rng, 4, 8)
		local shaped = gen.room(rng, 0, 0, sideRadius, pickShape(style, sideRadius, false, false, rng))
		local cells = plot:spur(rx, ry, wx, wy, length, shaped.radius + 3, if isOpen then 3 else 2, GAP, shaped, 5, rx, ry, 12, 1)
		if cells then
			branches[#branches + 1] = cells
			sideRooms[#sideRooms + 1] = shaped
			b += 1
		end
	end

	-- Loops: a corridor between two route cells far apart along it but near on the map (two
	-- diagonal runs, an L of 12 to 44, each run 3 or more, 30 or more along the route), a second way
	-- round. It may meet the blobs only round its two ends (14), and its middle keeps clear of the
	-- route.
	local loopsWanted = if segments >= 8 then 1 + segments // 8 else 0
	local endsOf = {} -- (each start's ends, found once: they depend on it alone)
	local made = 0
	attempts = 0
	while made < loopsWanted and attempts < 300 and n > 60 do
		attempts += 1
		local i = n // 8 + rng:next() % (n // 2) + 1
		local ends = endsOf[i]
		if not ends then
			ends = route:lEnds(i, 30, 8, 12, 44, 3)
			endsOf[i] = ends
		end
		if #ends > 0 then
			local j = ends[rng:next() % #ends + 1]
			local uFirst = rng:next() % 2 == 0
			local width = corridorWidth()
			local cells = plot:lRun(px[i], py[i], px[j], py[j], uFirst, width, width + 2, GAP, 14, route, width + 2, 6)
			if cells then
				loops[#loops + 1] = cells
				made += 1
			end
		end
	end

	-- The floor: every blob's (a room by its shape), then the rooms' holes closed (a ring's block, a
	-- pillared hall's pillars), each whole or not at all: not where a way runs through or past (two
	-- cells round the route, the branches and the loops) nor in the start chamber; then the lone
	-- cells floored.
	local ways = table.clone(path)
	for _, list in { branches, loops } do
		for _, cells in list do
			for _, c in cells do
				ways[#ways + 1] = c
			end
		end
	end
	return {
		open = plot:carve(ways, 2, key(start.x, start.y), start.radius + 2),
		path = path,
		branches = branches,
		loops = loops,
		start = start,
		finish = finish,
		chambers = chambers,
		sideRooms = sideRooms,
	}
end

return layout
