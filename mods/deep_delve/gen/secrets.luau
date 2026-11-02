-- An area's secrets (area.luau's hooks; the server's numbers come in params.secrets, server/
-- data/secrets.luau): now and then a secret room, and chests in the side rooms, some of them
-- trapped. Nothing in a lair, nothing in the town (the town is a section of its own; only the
-- area's side rooms are looked at).
--
-- The secret room is built from the game's own pieces, as the shipped quest maps build theirs: a
-- side room shut off by a straight line of secret wall across its branch (a wall group), and a
-- floor lever on the way nearby whose handler in the map's script opens the group ("Somewhere, a
-- secret wall has been opened."). Inside: a treasure chest at the room's back and a few spots for
-- its guards (the server puts them in). Shut, the line cuts that room alone off: the route, its
-- chambers, the end and every other side room are still reached from the start; open, the room is
-- reached through its branch again (the wall's cells are floor under it).
--
-- A locked secret room (by chance, in a biome with a door of its own): the line across its branch
-- is plain wall with the biome's door in it, locked with a silver key's lock, as the generated
-- levels' locked doors are built (a corridor four cells wide or more, three half the time, gets a
-- pair: one key opens both). No lever: the key is carried by a guard on the way near it (its spot
-- here; the server puts it in and drops the key at its death).
--
-- Side rooms' chests: each other side room a chest by chance, at its middle (the first free cell
-- out from it), and each chest trapped by chance, its trap drawn by weight (the server sets it off
-- when the chest is opened).
--
-- Three steps, all drawn from a stream of their own (so the area's other draws stay as they were
-- but for the cells the secrets take):
--   plan     before the walls are fitted: the room chosen, its line taken out of the floor
--   reserve  once the spots' used cells are made: the shut room's floor taken, so nothing of the
--            area's (a pack, the waypoint) is put in it
--   place    after the waypoint, before the packs: the secret walls, their group, the lever and
--            its handler, the chests; returns the spots.
--
-- params.secrets: { chance (an area's secret room, 0-1), chests (a side room's chest, 0-1),
-- trapped (a side-room chest's trap, 0-1), traps ({ {kind, weight}... }), hoard (the secret room's
-- chest types), guards ({ fewest, most }), locked (a secret room's lock in a biome with a door,
-- 0-1), doors ({ [biome name] = door type }: the biomes a room may be locked in) }.

local util = require("util")

local secrets = {}

local centre, worldX, worldY, chebyshev = util.centre, util.worldX, util.worldY, util.chebyshev

-- Script object values: the object whose handler runs.
local SELF = -2
-- A line across a branch reaches this far each way, and is at most this many cells.
local ACROSS, MOST_LINE = 8, 7
-- The shut room's floor at most (cells): a side room, never a stretch of the map.
local MOST_INSIDE = 700
-- The line's cells this far from the start and the end chambers at the least (beyond their own
-- radius): the ways home and on (their arches' back walls) are looked for near those chambers, so
-- the secret's walls never change what they find.
local FROM_START, FROM_END = 16, 10
-- The chests a biome names none of: the stone chests.
local STONE = { "CryptChest1", "CryptChest2", "CryptChest3", "CryptChest4" }

local function isStep(d: number): boolean
	return util.SIDE_DX[d] ~= nil
end

-- A whole-thousandths chance drawn from the stream.
local function chance(rng, p: number?): boolean
	return rng:next() % 1000 < math.floor((p or 0) * 1000 + 0.5)
end

local function point(c: number)
	return { x = worldX(c), y = worldY(c) }
end

-- A thing written as the town's pieces are: a plain one bare, any other with its own data.
local function put(w, thingType: string, c: number)
	if w:isPlain(thingType) then
		w:bare(thingType, worldX(c), worldY(c))
	else
		w:object(thingType, worldX(c), worldY(c))
	end
end

-- The line across side room i's branch that cuts that room alone off from the start (the route,
-- the chambers, the end and every other side room still reached), in the middle third of the
-- branch, clear of the room and far from the start and the end chambers; or nil.
local function plugFor(lay, i: number)
	local branch, room = lay.branches[i], lay.sideRooms[i]
	if not branch or not room or #branch < 6 then
		return nil
	end
	local open = lay.open
	local start, finish = centre(lay.start), centre(lay.finish)
	local nb = #branch
	-- (the room's middle, or where its branch ends in it when the middle is no floor: a pillar)
	local rc = centre(room)
	if not open:has(rc) then
		rc = branch[nb]
		if not open:has(rc) then
			return nil
		end
	end
	local k = nb // 3
	while k + 1 < nb and k <= nb * 2 // 3 do
		local here = branch[k + 1]
		local d = branch[k + 2] - here
		if isStep(d) then
			local line = open:lineAcross(here, d, ACROSS)
			local fine = #line > 0 and #line <= MOST_LINE and chebyshev(here, rc) > room.radius + 1
			for _, c in line do
				fine = fine and chebyshev(c, start) >= lay.start.radius + FROM_START and chebyshev(c, finish) >= lay.finish.radius + FROM_END
			end
			if fine then
				local reach = open:reach(start, line)
				fine = not reach:has(rc) and reach:has(finish)
				-- (everything else still reached: the route, the chambers, the other side rooms)
				if fine then
					for _, c in lay.path do
						if open:has(c) and not reach:has(c) then
							fine = false
							break
						end
					end
				end
				if fine then
					for _, ch in lay.chambers do
						if not reach:has(centre(ch)) then
							fine = false
							break
						end
					end
				end
				if fine then
					for j, sr in lay.sideRooms do
						if j ~= i and not reach:has(centre(sr)) then
							fine = false
							break
						end
					end
				end
				if fine then
					return line, rc, k
				end
			end
		end
		k += 1
	end
	return nil
end

-- A locked room's door across side room i's branch at its line (`k`: the branch cell before the
-- line's, from 0 as plugFor counts): the gap (the branch's cell in the line, and with a pair the
-- next along the line), each leaf's hinge and shut direction, the line's walls (the line but the
-- gap), and two branch cells either side of the gap (`outside` reached from the start shut, `inside`
-- the room's side). As the generated levels' locked doors: a "/" line (the branch stepping
-- (1, -1) or (-1, 1)) shuts its leaves at 8 / 0x18, a "\" one at 16 / 0.
local function lockFor(lay, i: number, line: { number }, k: number, inside, rng, doorType: string)
	local branch = lay.branches[i]
	local here = branch[k + 1]
	local d = branch[k + 2] - here
	local slash = util.SIDE_DX[d] == util.SIDE_DY[d]
	local along = if slash then util.ROW - 1 else util.ROW + 1
	local gaps = { here }
	if #line >= 4 or rng:next() % 2 == 0 then
		if util.find(line, here + along) then
			gaps[2] = here + along
		elseif util.find(line, here - along) then
			table.insert(gaps, 1, here - along)
		end
	end
	local walls = {}
	for _, c in line do
		if not util.find(gaps, c) then
			table.insert(walls, c)
		end
	end
	local gx, gy = util.xy(gaps[1])
	local leaves = { { direction = if slash then 8 else 16, hingeX = gx, hingeY = if slash then gy + 1 else gy } }
	if #gaps == 2 then
		local hx, hy = util.xy(gaps[2])
		leaves[2] = { direction = if slash then 0x18 else 0, hingeX = hx + 1, hingeY = if slash then hy else hy + 1 }
	end
	-- (two branch cells off the gap either way; the one the shut floor holds is the inside)
	local a, b = branch[math.max(1, k - 1)], branch[math.min(#branch, k + 3)]
	if inside:has(a) then
		a, b = b, a
	end
	return { type = doorType, gaps = gaps, walls = walls, leaves = leaves, outside = a, inside = b }
end

-- The plan (p: the area's params; `open`: the map's open floor, the area's and the town's): the
-- secret room's line taken out of both floors when an area has one (a locked room's gap given back:
-- its door stands there). Returns the plan (nil in a lair or with no numbers): { rng, numbers, room,
-- index (its side room's), plug (the line's cells), inside (the shut floor, gen.cells), lock (a
-- locked room's door: lockFor's) }.
function secrets.plan(lay, open, p)
	local s = p.secrets
	if p.lair or type(s) ~= "table" then
		return nil
	end
	local rng = gen.minstd(util.hash(p.seed, 2246822519, 47))
	local plan: any = { rng = rng, numbers = s }
	-- (the roll always drawn first: the chests' draws come after it either way)
	if chance(rng, s.chance) and #lay.sideRooms > 0 then
		local order = {}
		for i = 1, #lay.sideRooms do
			order[i] = i
		end
		rng:shuffle(order)
		for _, i in order do
			local line, inner, k = plugFor(lay, i)
			if line and inner and k then
				local room = lay.sideRooms[i]
				lay.open:removeAll(line)
				open:removeAll(line)
				local inside = lay.open:reach(inner)
				local n = inside:count()
				if n > 0 and n <= MOST_INSIDE then
					plan.room, plan.index, plan.plug, plan.inside = room, i, line, inside
					-- Locked (by chance, in a biome with a door; the roll drawn only there): its gap
					-- floor again, the door's.
					local doorType = if type(s.doors) == "table" then s.doors[p.biome] else nil
					if type(doorType) == "string" and chance(rng, s.locked) then
						plan.lock = lockFor(lay, i, line, k, inside, rng, doorType)
						lay.open:addAll(plan.lock.gaps)
						open:addAll(plan.lock.gaps)
					end
					break
				end
				-- (too much behind it: put back, the next one tried)
				lay.open:addAll(line)
				open:addAll(line)
			end
		end
	end
	return plan
end

-- The secret walls' cells (nothing hangs on them), for the back walls; none: an empty list.
function secrets.walls(plan): { number }
	return if plan and plan.plug then plan.plug else {}
end

-- The side rooms the area's own things may use: all but the secret room.
function secrets.others(plan, sideRooms)
	if not plan or not plan.room then
		return sideRooms
	end
	local out = {}
	for _, room in sideRooms do
		if room ~= plan.room then
			table.insert(out, room)
		end
	end
	return out
end

-- The shut room's floor and a cell round its walls taken in `used` (what was free before kept for
-- its own chest and guards).
function secrets.reserve(plan, used)
	if not plan or not plan.room then
		return
	end
	plan.free = used:clone()
	used:addAll(plan.inside:list())
	for _, c in plan.plug do
		used:addSquare(c, 1)
	end
end

-- The secret room's pieces: its walls secret in one group, its chest, its guards' spots, its
-- lever and the lever's handler. Its spot: { x, y (the room's middle), group, cells (the walls'
-- cells as { x, y } on the wall grid), chest = { x, y, type }, lever = { x, y } (none: open from
-- the start, nowhere for a lever), guards = { { x, y }... } }.
--
-- A locked room instead: its line plain wall (fitted with the area's walls), its door's leaves in
-- the gap, shut and locked (a silver key's lock); no lever. Its spot's `lock` = { type, doors =
-- { { x, y } each leaf's hinge }, gaps = { { x, y } on the wall grid }, outside, inside (world
-- points either side of the gap), guard (the key guard's spot, on the way near it) }; `cells` the
-- line's walls.
local function placeRoom(plan, site, w)
	local rng, s = plan.rng, plan.numbers
	local lay, used = site.layout, site.used
	local name = "DDSecret1"
	local lock = plan.lock
	local spot: any = { cells = {}, guards = {} }
	if lock then
		spot.lock = { type = lock.type, doors = {}, gaps = {}, outside = point(lock.outside), inside = point(lock.inside) }
		for _, leaf in lock.leaves do
			w:thing(lock.type, leaf.hingeX * 23, leaf.hingeY * 23, { door = { direction = leaf.direction, lock = 1 } })
			table.insert(spot.lock.doors, { x = leaf.hingeX * 23, y = leaf.hingeY * 23 })
		end
		for _, c in lock.gaps do
			local cx, cy = util.xy(c)
			table.insert(spot.lock.gaps, { x = cx, y = cy })
		end
		for _, c in lock.walls do
			local cx, cy = util.xy(c)
			table.insert(spot.cells, { x = cx, y = cy })
		end
	else
		for _, c in plan.plug do
			w:secretWall(c, false)
		end
		w:wallGroup(name, plan.plug)
		spot.group = name
		for _, c in plan.plug do
			local cx, cy = util.xy(c)
			table.insert(spot.cells, { x = cx, y = cy })
		end
	end
	local rc = centre(plan.room)
	local here = point(rc)
	spot.x, spot.y = here.x, here.y
	-- The line's middle (its cell nearest the route's start: where it's seen from).
	local start = centre(lay.start)
	local mid = plan.plug[1]
	for _, c in plan.plug do
		if chebyshev(c, start) < chebyshev(mid, start) then
			mid = c
		end
	end
	-- The chest: at the room's back, the free cell (free sides too) farthest from the line within
	-- the room's reach of its middle.
	local free = plan.free
	local cells = plan.inside:list()
	local best, bestD = nil, -1
	for _, c in cells do
		if chebyshev(c, rc) <= plan.room.radius and gen.unusedAround(lay.open, free, c) then
			local d = chebyshev(c, mid)
			if d > bestD or (d == bestD and c < (best :: number)) then
				best, bestD = c, d
			end
		end
	end
	if best then
		free:addSquare(best, 1)
		local hoard = s.hoard or { "Chest1" }
		local t = hoard[rng:next() % #hoard + 1]
		put(w, t, best)
		local at = point(best)
		spot.chest = { x = at.x, y = at.y, type = t }
	end
	-- Its guards: free cells in the room, two apart, toward its middle.
	local g = s.guards or { 0, 0 }
	local want = g[1] + (if g[2] > g[1] then rng:next() % (g[2] - g[1] + 1) else 0)
	local tries = 0
	while #spot.guards < want and tries < 80 do
		tries += 1
		local c = cells[rng:next() % #cells + 1]
		if chebyshev(c, rc) <= math.max(2, plan.room.radius - 1) and gen.unusedAround(lay.open, free, c) then
			free:addSquare(c, 1)
			table.insert(spot.guards, point(c))
		end
	end
	-- A locked room's key guard: on the route a little way from the line, else further along it,
	-- else the first free cell round the line (never inside: that's taken).
	if lock then
		local gc = nil
		for _, r in { { 5, 16 }, { 3, 26 } } do
			local list = gen.spotsNear(lay.path, mid, r[1], r[2], lay.open, used, start, 10)
			if #list > 0 then
				gc = list[rng:next() % #list + 1]
				break
			end
		end
		if not gc then
			gc = gen.firstAround(mid, 2, 20, lay.open, used, true)
		end
		-- (nowhere at all: by the door's outside, never seen)
		spot.lock.guard = if gc then point(gc) else spot.lock.outside
		if gc then
			used:addSquare(gc, 2)
		end
		return spot
	end
	-- The lever: on the route within a few cells of the line, else further along it, else the first
	-- free cell round the line (never inside: that's taken).
	local script = w:script()
	local lc = nil
	for _, reach in { 14, 24 } do
		local list = gen.spotsNear(lay.path, mid, 3, reach, lay.open, used, start, 10)
		if #list > 0 then
			lc = list[rng:next() % #list + 1]
			break
		end
	end
	if not lc then
		lc = gen.firstAround(mid, 2, 20, lay.open, used, true)
	end
	if lc then
		used:addSquare(lc, 1)
		local fn = name .. "Lever"
		local at = point(lc)
		w:thing("Lever1", at.x, at.y, { trigger = { size = 0, pressed = fn } })
		-- (thrown once: off for good, the wall opened, everyone told as the quest maps tell it)
		script:fn(fn)
			:pushInt(SELF):call("ObjectOff")
			:pushString(name):call("WallGroup"):call("WallGroupOpen")
			:pushString("GeneralPrint:MsgFarWallOpen"):call("PrintToAll")
		spot.lever = at
	else
		-- (nowhere for a lever: open from the start, a plain side room with its chest)
		script:fn("MapInitialize" .. name):pushString(name):call("WallGroup"):call("WallGroupOpen")
		spot.open = true
	end
	return spot
end

-- The side rooms' chests (every side room but the secret one), each by chance, trapped by chance.
-- Each: { x, y, type, trap (its kind, or nil) }.
local function placeChests(plan, site, w)
	local rng, s = plan.rng, plan.numbers
	local lay, used = site.layout, site.used
	local types = if site.biome.chests and #site.biome.chests > 0 then site.biome.chests else STONE
	local weights = 0
	for _, t in s.traps or {} do
		weights += t[2]
	end
	local out = {}
	for _, room in secrets.others(plan, lay.sideRooms) do
		if chance(rng, s.chests) then
			local rc = centre(room)
			local c = gen.firstAround(rc, 0, math.max(1, room.radius - 2), lay.open, used, true)
			if c then
				used:addSquare(c, 1)
				local t = types[rng:next() % #types + 1]
				put(w, t, c)
				local chest: any = point(c)
				chest.type = t
				if weights > 0 and chance(rng, s.trapped) then
					local roll = rng:next() % weights
					for _, k in s.traps do
						roll -= k[2]
						if roll < 0 then
							chest.trap = k[1]
							break
						end
					end
				end
				table.insert(out, chest)
			end
		end
	end
	return out
end

-- The secrets written (site: layout, used, biome): the secret room's spot (or nil) and the side
-- rooms' chests.
function secrets.place(plan, site, w): (any, { any })
	if not plan then
		return nil, {}
	end
	local room = if plan.room then placeRoom(plan, site, w) else nil
	return room, placeChests(plan, site, w)
end

return secrets
