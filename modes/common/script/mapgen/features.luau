-- A generated level's traps and puzzles, stamped in the way
-- the shipped maps build theirs (the quest maps G_Temple, G_Crypts, G_Castle; War08d, Con04c): their
-- objects, wired by the map's script, every function named after the thing it serves. The design is
-- here; the scans over cells are native (gen.cells, gen.backs, gen.spotsNear, gen.firstAround...).
--
-- Traps, along the route away from the start and the end:
--   spikes   a band of PeriodicSpike across a corridor (Wiz02C, War07G): no script.
--   glyph    a Glyph with one spell in a corridor (G_LOTD, G_Castle): goes off on the first one
--            over it; no script.
--   arrows   ArrowTraps (off) on the back walls beside a corridor, invisible Triggers across it
--            ahead of them: stepping on one turns the traps on for a frame (each shoots once) and
--            the triggers off for 2.5 s (G_Temple's ArrowTrap rows, War08d).
--   fist     an invisible Trigger: a Fist of Vengeance on whoever steps on it, then off for 3 s
--            (G_Crypts' DropFist).
--   skulls   fire skulls on a chamber's back walls (G_Castle): always on; deep levels only.
--   crumble  a patch of PitCrumblingFloor three cells square on the route in a chamber
--            (G_Forest), where the floor round it still reaches everything: stepped on, it gives
--            way and drops whoever's on it some way back along the route.
--   crusher  a SpikeBlock shuttling across a corridor on its Mover between two waypoints linked
--            both ways, set going at the map's start (G_Temple's SpikeBlock02-04: one Move()).
-- Locked doors: a line of wall across the route with a gap, the biome's door in it (a silver key's
-- lock), the key on the start's side of it (G_Crypts' and G_Castle's doors). A corridor four cells
-- wide or more (three, half the time) gets a pair: two cells, a door hinged at each end, their
-- leaves meeting in the middle (Estate's, G_Castle's); one key opens both (DoorCollide's pair).
-- Puzzles, a vault each (a side room shut by a straight line of secret wall across its branch, a
-- chest and a reward marker inside):
--   lever    a floor lever somewhere on the route opens it (G_Castle's OpenWallRing, "Somewhere, a
--            secret wall has been opened.").
--   runes    three pressure plates in the rooms before it; each goes down once stepped on, the
--            third opens it (G_Castle's SecretDown, counting).
-- Ambushes: a route chamber's ways in and out are secret walls standing open; invisible Triggers
-- inside shut them, and its monsters' deaths count down to open them again ("A wall has been
-- lowered."), two minutes at the most (G_Crypts' Ambush, Con04c's SkeletonAmbush).
--
-- The draws come from the features' own stream (made in plan, drawn again in place) in the order
-- the C++ generator made them.

local util = require("util")

local features = {}

local ROW = util.ROW
local SIDE_DX, SIDE_DY = util.SIDE_DX, util.SIDE_DY
local chebyshev = util.chebyshev
local centre = util.centre
local worldX, worldY = util.worldX, util.worldY
local abs, max, min = math.abs, math.max, math.min

-- Script object values: the object whose handler runs.
local SELF = -2
-- A line across a corridor reaches this far each way.
local ACROSS = 8

-- The open cells in a straight line across a corridor going `d` (a side step) at `at` (less
-- `gone`, a gen.cells, when given).
local function lineAcross(open, at: number, d: number, gone)
	return open:lineAcross(at, d, ACROSS, gone)
end

local function isStep(d: number): boolean
	return SIDE_DX[d] ~= nil
end

-- Up to `count` side rooms to shut: each by a straight line across its branch that cuts that room
-- alone off from the start (everything else still reached). Each: {room, plug (the wall cells)}.
local function planVaults(lay, count: number, rng)
	local out = {}
	local order = {}
	for i = 1, #lay.sideRooms do
		order[i] = i
	end
	rng:shuffle(order)
	local open = lay.open:clone()
	local start, finish = centre(lay.start), centre(lay.finish)
	local shut = {}
	for _, i in order do
		if #out >= count or #lay.branches < i then
			break
		end
		local branch = lay.branches[i]
		local room = lay.sideRooms[i]
		local rc = centre(room)
		local nb = #branch
		if nb >= 6 then
			-- A narrow line across the branch in its middle third, clear of the room itself.
			local k = nb // 3 -- (from 0, as the C++ counted)
			while k + 1 < nb and k <= nb * 2 // 3 do
				local here = branch[k + 1]
				local d = branch[k + 2] - here
				if isStep(d) then
					local line = lineAcross(open, here, d)
					if #line > 0 and #line <= 7 and chebyshev(here, rc) > room.radius + 1 then
						-- Shut the line: only this room may be cut off, and the end must still be reached.
						local reach = open:reach(start, line)
						if not reach:has(rc) and reach:has(finish) then
							-- Everything else still reached: the route, the chambers, the other side rooms.
							local ok = true
							for _, c in lay.path do
								if open:has(c) and not reach:has(c) then
									ok = false
									break
								end
							end
							if ok then
								for _, ch in lay.chambers do
									if not reach:has(centre(ch)) then
										ok = false
										break
									end
								end
							end
							if ok then
								for j, sr in lay.sideRooms do
									if j ~= i and not util.find(shut, j) and not reach:has(centre(sr)) then
										ok = false
										break
									end
								end
							end
							if ok then
								out[#out + 1] = { room = room, plug = line }
								shut[#shut + 1] = i
								open:removeAll(line)
								break
							end
						end
					end
				end
				k += 1
			end
		end
	end
	return out
end

-- Up to `count` route chambers (not the start nor the end) that two lines across the route seal off
-- from both the start and the end. Each: {room, walls (the lines, in then out: secret, standing
-- open), triggers (a line across the way in, three cells inside the wall), inside (a gen.cells: the
-- floor the walls shut in)}.
local function planAmbushes(lay, count: number, rng)
	local out = {}
	local chambers = lay.chambers
	if #chambers < 3 then
		return out
	end
	local order = {}
	for i = 2, #chambers - 1 do
		order[#order + 1] = i
	end
	rng:shuffle(order)
	local open = lay.open:clone()
	local path = lay.path
	local n = #path
	local start, finish = centre(lay.start), centre(lay.finish)
	for _, i in order do
		if #out >= count then
			break
		end
		local room = chambers[i]
		local rc = centre(room)
		local ic = util.find(path, rc)
		if ic then
			-- The nearest narrow line across the route outside the room, before it and after it.
			local function lineAt(k: number)
				if k >= n then
					return nil
				end
				local d = path[k + 1] - path[k]
				if not isStep(d) then
					return nil
				end
				local line = lineAcross(open, path[k], d)
				if #line == 0 or 7 < #line then
					return nil
				end
				for _, c in line do
					if chebyshev(c, rc) <= room.radius + 1 then
						return nil
					end
				end
				return line
			end
			-- The nearest narrow lines across the route within 30 cells before the room (the way in)
			-- and after it (the way out).
			local into, kin = nil, 0
			for k = ic - 1, max(2, ic - 30), -1 do
				into = lineAt(k)
				if into then
					kin = k
					break
				end
			end
			local outOf = nil
			local k = ic + 1
			while k < n and k - ic <= 30 do
				outOf = lineAt(k)
				if outOf then
					break
				end
				k += 1
			end
			if into and outOf and kin + 3 < ic then
				local walls = table.clone(into)
				for _, c in outOf do
					walls[#walls + 1] = c
				end
				-- What the two lines enclose must hold neither the start nor the end, and not be huge.
				local inside = open:reach(rc, walls)
				if not inside:has(start) and not inside:has(finish) and inside:count() <= 2500 then
					-- The triggers: across the way in, three cells past its wall (all inside).
					local d = path[kin + 4] - path[kin + 3]
					local triggers = if isStep(d) then lineAcross(open, path[kin + 3], d, gen.cells(walls)) else {}
					local fine = #triggers > 0 and #triggers <= 20
					if fine then
						for _, c in triggers do
							if not inside:has(c) then
								fine = false
								break
							end
						end
					end
					if fine then
						out[#out + 1] = { room = room, walls = walls, triggers = triggers, inside = inside }
						open:removeAll(walls)
					end
				end
			end
		end
	end
	return out
end

-- Up to `count` doors across the route, each one the end is reached only through. Each: {walls (the
-- line but the gap), leaves ({gap, direction (shut: 8 / 0x18 in a "/" line, 16 / 0 in a "\"),
-- hingeX, hingeY (the hinge's grid corner)}: one door, or a pair), key (where the key lies)}.
local function planDoors(lay, count: number, rng)
	local out = {}
	local path = lay.path
	local n = #path
	if n < 40 then
		return out
	end
	local open = lay.open:clone()
	local start, finish = centre(lay.start), centre(lay.finish)
	local tries = 0
	while tries < 80 and #out < count do
		tries += 1
		-- A route cell between two fifths and four fifths of the way along (from 0).
		local k = n * 2 // 5 + rng:next() % (n * 2 // 5)
		if k + 1 < n then
			local here = path[k + 1]
			local d = path[k + 2] - here
			if isStep(d) then
				local line = lineAcross(open, here, d)
				local fine = #line >= 3 and #line <= 7 and chebyshev(here, finish) > lay.finish.radius + 3
				if fine then
					for _, o in out do
						if chebyshev(o.leaves[1].gap, here) < 12 then
							fine = false
							break
						end
					end
				end
				if fine then
					-- The line across goes along (d.x, -d.y): "/" for (1, -1) or (-1, 1), "\" else. Four
					-- cells or more across (three, half the time), a pair: the route's cell and the next
					-- along the line (the first's leaf from its own corner, the second's from the far
					-- one, meeting between).
					local slash = SIDE_DX[d] == SIDE_DY[d]
					local along = if slash then ROW - 1 else ROW + 1
					local gaps = { here }
					if #line >= 4 or rng:next() % 2 == 0 then
						if util.find(line, here + along) then
							gaps[2] = here + along
						elseif util.find(line, here - along) then
							table.insert(gaps, 1, here - along)
						end
					end
					-- Shut, the door must cut the end off; the key goes where the start reaches then.
					local reach = open:reach(start, line)
					if not reach:has(finish) then
						local keySpots = {}
						for _, room in lay.sideRooms do -- (a side room's, the explorer's find)
							local c = centre(room)
							if reach:has(c) and chebyshev(c, start) >= 10 then
								keySpots[#keySpots + 1] = c
							end
						end
						if #keySpots == 0 then
							for j = 1, k - 8 do -- (else on the route, well before it)
								local c = path[j]
								if chebyshev(c, start) >= 10 and chebyshev(c, here) >= 8 and reach:has(c) then
									keySpots[#keySpots + 1] = c
								end
							end
						end
						if #keySpots > 0 then
							local door = { walls = {}, leaves = {} }
							for _, c in line do
								if not util.find(gaps, c) then
									door.walls[#door.walls + 1] = c
								end
							end
							-- (World::load's leaf cell: the hinge plus half its shut swing.)
							local gx, gy = util.xy(gaps[1])
							door.leaves[1] = { gap = gaps[1], direction = if slash then 8 else 16, hingeX = gx, hingeY = if slash then gy + 1 else gy }
							if #gaps == 2 then
								local hx, hy = util.xy(gaps[2])
								door.leaves[2] = {
									gap = gaps[2],
									direction = if slash then 0x18 else 0,
									hingeX = hx + 1,
									hingeY = if slash then hy else hy + 1,
								}
							end
							door.key = keySpots[rng:next() % #keySpots + 1]
							open:removeAll(door.walls)
							out[#out + 1] = door
						end
					end
				end
			end
		end
	end
	return out
end

-- The level's vaults, doors and ambushes (p: winding, puzzles, doors, ambushes, seed), their cells
-- taken out of lay.open (the floor walked: walls, the floor under them the base). Returns
-- {secret (the secret walls' cells: nothing hangs on them), vaultRooms, and the planning's own
-- results for the placing (passed back to features.place as site.plan): nothing kept here
-- between the two).
function features.plan(lay, p)
	local rng = gen.minstd(util.hash(p.seed, 40503, 7))
	local vaults, doors, ambushes = {}, {}, {}
	if p.winding and p.puzzles > 0 then
		vaults = planVaults(lay, p.puzzles, rng)
	end
	for _, v in vaults do
		lay.open:removeAll(v.plug)
	end
	if p.winding and p.doors > 0 then
		doors = planDoors(lay, p.doors, rng)
	end
	for _, d in doors do
		lay.open:removeAll(d.walls)
	end
	if p.winding and p.ambushes > 0 then
		ambushes = planAmbushes(lay, p.ambushes, rng)
	end
	for _, a in ambushes do
		lay.open:removeAll(a.walls)
	end
	local secret, vaultRooms = {}, {}
	for _, v in vaults do
		vaultRooms[#vaultRooms + 1] = v.room
		for _, c in v.plug do
			secret[#secret + 1] = c
		end
	end
	for _, a in ambushes do
		for _, c in a.walls do
			secret[#secret + 1] = c
		end
	end
	return {
		secret = secret,
		vaultRooms = vaultRooms,
		lay = lay,
		rng = rng,
		vaults = vaults,
		doors = doors,
		ambushes = ambushes,
	}
end

local MILD = { "SPELL_SLOW", "SPELL_CONFUSE", "SPELL_POISON" }
local HARD = { "SPELL_FIREBALL", "SPELL_SHOCK", "SPELL_METEOR" }
local SPIKES, GLYPH, ARROWS, FIST, CRUMBLE, CRUSHER = 1, 2, 3, 4, 5, 6
local TRAP_NAMES = { "spikes", "glyph", "arrows", "fist", "crumble", "crusher" }

-- Places every planned and random feature on the writer `w` (site: plan (what features.plan
-- returned), walls (a gen.cellmap of the walls' shapes), backs (gen.backs), used (gen.cells: the
-- cells taken, added to), doorType, obelisks, obeliskType, monsters (an ambush takes its own from
-- the end), traps, depth), drawing from the planning's stream. Returns {monsters (what's left), placed (each feature's kind as it
-- went in)}.
function features.place(site, w)
	local state = site.plan
	local lay, rng = state.lay, state.rng
	local open = lay.open
	local path = lay.path
	local used = site.used
	local monsters = site.monsters
	local placed = {}
	local script = w:script()
	local start, finish = centre(lay.start), centre(lay.finish)
	local serial = 0
	local function nameOf(kind: string): string
		serial += 1
		return kind .. tostring(serial)
	end
	local function note(kind: string)
		placed[#placed + 1] = kind
	end
	local function unused(c: number): boolean
		return open:has(c) and not used:has(c)
	end
	-- A free floor cell with free floor on every side.
	local function unusedAround(c: number): boolean
		return gen.unusedAround(open, used, c)
	end
	-- A free floor cell with free sides, from..to from `near`, on the route if it can be.
	local function spotNear(near: number, from: number, to: number)
		local best = gen.spotsNear(path, near, from, to, open, used, start, 4)
		if #best == 0 then
			return nil
		end
		return best[rng:next() % #best + 1]
	end
	local function claim(c: number, around: number)
		used:addSquare(c, around)
	end
	-- Rearming a trigger (its script id the timer's argument): shared by the traps.
	local function rearm(): string
		if not script:has("TrapRearm") then
			script:fn("TrapRearm", 1):loadLocal(0):call("ObjectOn")
		end
		return "TrapRearm"
	end
	local function at(c: number): (number, number)
		return worldX(c), worldY(c)
	end
	-- A trigger of `size` that calls the script function `pressed` when a player steps on it.
	local function plate(size: number, pressed: string)
		return { size = size, pressed = pressed }
	end
	-- Where a thing on back wall `wc` stands, a fifth of a cell out toward the open cell `front`
	-- before it (on the wall's line: a torch's place).
	local fifth = gen.f32(gen.f32(0.2) * 23)
	local function onWall(wc: number, front: number): (number, number)
		local d = front - wc
		return gen.f32(worldX(wc) + fifth * SIDE_DX[d]), gen.f32(worldY(wc) + fifth * SIDE_DY[d])
	end

	-- A vault: its walls secret, a reward inside, and its puzzle.
	local function placeVault(v)
		local vault = nameOf("Vault")
		for _, c in v.plug do
			w:secretWall(c, false)
		end
		w:wallGroup(vault, v.plug)
		local rc = centre(v.room)
		if unused(rc) then
			used:add(rc)
			w:thing("RewardMarkerPlus", worldX(rc), worldY(rc), {})
		end
		local mid = v.plug[1]
		for _, c in v.plug do
			if chebyshev(c, start) < chebyshev(mid, start) then
				mid = c
			end
		end
		local runes = rng:next() % 2 == 0
		if runes then
			-- Three plates in the rooms before it, each down once stepped on; the third opens it.
			local spots = {}
			local tries = 0
			while tries < 60 and #spots < 3 do
				tries += 1
				local c = spotNear(mid, 4 + 2 * #spots, 18)
				if c then
					local apart = true
					for _, a in spots do
						if chebyshev(a, c) < 4 then
							apart = false
							break
						end
					end
					if apart then
						spots[#spots + 1] = c
						claim(c, 1)
					end
				end
			end
			if #spots == 3 then
				local count = script:global(vault .. "Runes")
				local fn = vault .. "Rune"
				for _, c in spots do
					local x, y = at(c)
					w:thing("PressurePlate", x, y, { trigger = plate(40, fn) })
				end
				-- Each plate switches itself off and counts; the third opens the vault.
				script:fn(fn)
					:pushInt(SELF):call("ObjectOff")
					:storeGlobal(count):loadGlobal(count):pushInt(1):op("IntAdd"):endStore()
					:loadGlobal(count):pushInt(3):op("IntLt"):jumpIf("wait")
					:pushString(vault):call("WallGroup"):call("WallGroupOpen")
					:pushString("GeneralPrint:MsgSeWallOpen"):call("PrintToAll")
					:label("wait")
				note("runes")
				return
			end
		end
		-- A lever further off on the route.
		local c = spotNear(mid, 8, 30)
		if c then
			claim(c, 1)
			local fn = vault .. "Lever"
			local x, y = at(c)
			w:thing("Lever1", x, y, { trigger = plate(0, fn) })
			script:fn(fn)
				:pushInt(SELF):call("ObjectOff")
				:pushString(vault):call("WallGroup"):call("WallGroupOpen")
				:pushString("GeneralPrint:MsgFarWallOpen"):call("PrintToAll")
			note("lever")
		else
			-- (Nowhere for its puzzle: open from the start, a plain side room.)
			script:fn("MapInitialize" .. vault):pushString(vault):call("WallGroup"):call("WallGroupOpen")
			note("open vault")
		end
	end

	-- Mana obelisks: each in a chamber of the route (not the start's nor the end's), near its middle,
	-- a different chamber each.
	local function placeObelisks()
		local chambers = lay.chambers
		if site.obelisks <= 0 or #chambers <= 2 then
			return
		end
		local rooms = {}
		for i = 2, #chambers - 1 do
			rooms[#rooms + 1] = i
		end
		rng:shuffle(rooms)
		local placedHere = 0
		for _, i in rooms do
			if placedHere == site.obelisks then
				break
			end
			local room = chambers[i]
			local c = gen.firstAround(centre(room), 2, max(2, room.radius - 2), open, used, true)
			if c then
				claim(c, 1)
				local x, y = at(c)
				w:thing(site.obeliskType, x, y, {})
				note("obelisk")
				placedHere += 1
			end
		end
	end

	-- A locked door: the door on its hinge corner, shut, a silver key's lock; the key before it.
	local function placeDoor(kd)
		for _, leaf in kd.leaves do
			claim(leaf.gap, 1)
			w:thing(site.doorType, leaf.hingeX * 23, leaf.hingeY * 23, { door = { direction = leaf.direction, lock = 1 } })
		end
		local c = gen.firstAround(kd.key, 0, 4, open, used, false)
		if c then
			used:add(c)
			local x, y = at(c)
			w:thing("SilverKey", x, y, {})
		end
		note("door")
	end

	-- An ambush: the room's own monsters (the level's, from the end of its list), each one's death
	-- counted; the walls shut as the party comes in, open as the last dies.
	local function placeAmbush(a)
		local name = nameOf("Ambush")
		for _, c in a.walls do
			w:secretWall(c, true)
			claim(c, 1)
		end
		w:wallGroup(name, a.walls)
		-- Spots for the monsters: deep in, away from the triggers, apart.
		local spots = {}
		local inside = a.inside:list()
		rng:shuffle(inside)
		-- (C's truncating division; a negative cap was a size_t's wrap, so no cap: below depth -7.)
		local cap = 3 + util.idiv(site.depth, 2)
		local wanted = min(#monsters // 3, if cap < 0 then 8 else cap, 8)
		for _, c in inside do
			if #spots >= wanted then
				break
			end
			if unusedAround(c) then
				local fine = true
				for _, t in a.triggers do
					if chebyshev(t, c) < 4 then
						fine = false
						break
					end
				end
				if fine then
					for _, s in spots do
						if chebyshev(s, c) < 2 then
							fine = false
							break
						end
					end
				end
				if fine then
					spots[#spots + 1] = c
				end
			end
		end
		if #spots < 2 then
			return -- (no room for them: the walls stand open, nothing shuts them)
		end
		local left = script:global(name .. "Left")
		local done = script:global(name .. "Done")
		script:setGlobalAtLoad(left, #spots)
		for _, c in spots do
			local type = monsters[#monsters]
			monsters[#monsters] = nil
			local x, y = at(c)
			w:thing(type, x, y, { onDeath = name .. "Death" })
			claim(c, 1)
		end
		local triggers = {}
		for _, c in a.triggers do
			local x, y = at(c)
			triggers[#triggers + 1] =
				w:thing("Trigger", x, y, { name = name .. "Trigger" .. tostring(#triggers), trigger = plate(30, name .. "Start") })
			used:add(c)
		end
		w:objectGroup(name .. "Triggers", triggers)
		-- A lever outside, on the route a few cells before the way in (so one who fell and came back,
		-- or stayed out, opens it again for the rest): once the walls have shut, it opens them;
		-- before, it does nothing (no disarming the ambush).
		local sprungAt = script:global(name .. "Sprung")
		local kin = #path -- (from 0: the route's first cell in the walls)
		for k = 1, #path do
			if util.find(a.walls, path[k]) then
				kin = k - 1
				break
			end
		end
		local lever = nil
		local back = 3
		while back <= 8 and back <= kin and not lever do
			local c = path[kin - back + 1]
			if not a.inside:has(c) and unusedAround(c) then
				lever = c
			end
			back += 1
		end
		if lever then
			claim(lever, 1)
			-- (Thrown and thrown back alike: a lever toggles, its pressed handler one way, its released
			-- the other; one thrown too soon still opens the walls the next time.)
			local x, y = at(lever)
			w:thing("Lever1", x, y, { trigger = { size = 0, pressed = name .. "Lever", released = name .. "Lever" } })
			script:fn(name .. "Lever")
				:loadGlobal(sprungAt):pushInt(1):op("IntLt"):jumpIf("end")
				:callScript(name .. "Open")
				:label("end")
			note("ambush lever")
		end
		-- Stepping in: triggers off; unless already over, the walls shut, opening anyway after 120 s.
		script:fn(name .. "Start")
			:pushString(name .. "Triggers"):call("ObjectGroup"):call("ObjectGroupOff")
			:loadGlobal(done):jumpIf("end")
			:setGlobal(sprungAt, 1)
			:pushString(name):call("WallGroup"):call("WallGroupClose")
			:pushInt(120):pushFunction(name .. "Open"):call("SecondTimer")
			:label("end")
		-- Each death counts down; the last opens the walls.
		script:fn(name .. "Death")
			:storeGlobal(left):loadGlobal(left):pushInt(1):op("IntSub"):endStore()
			:loadGlobal(left):pushInt(0):op("IntGt"):jumpIf("wait")
			:callScript(name .. "Open")
			:label("wait")
		-- Opens the walls once, however it is reached (last death, timer or lever).
		script:fn(name .. "Open")
			:loadGlobal(done):jumpIf("end")
			:setGlobal(done, 1)
			:pushString(name):call("WallGroup"):call("WallGroupOpen")
			:pushString("GeneralPrint:MsgWallLowered"):call("PrintToAll")
			:label("end")
		note("ambush")
	end

	-- The floor's cells reached from the start (side steps), without `gone`: a crumbling floor, once
	-- given way, stays a hole (TrapDoorUpdate), so it goes only where everything reached before is
	-- reached round it.
	local function reached(gone): number
		return open:reach(start, gone):count()
	end
	local reachedAll = nil
	local sprung = {} -- (the crumbling floors placed so far: a list)

	-- Rotten planks on the route in a chamber: a patch three cells square, the chamber's floor round
	-- it. Whoever steps on it falls through, landing a dozen cells back along the route (G_Forest's
	-- PitCrumblingFloor); the way round it stays. False where it can't go.
	local function placeCrumble(k: number, c: number): boolean
		local cx, cy = util.xy(c)
		local inChamber = false
		for _, r in lay.chambers do
			if r.radius >= 4 and abs(cx - r.x) + abs(cy - r.y) <= r.radius - 2 then
				inChamber = true
				break
			end
		end
		-- Where the faller lands: a dozen cells back along the route.
		local land = path[if k - 1 >= 12 then k - 12 else 1]
		-- (three side steps each way: a square on screen, the cells (a + b, a - b) round it for a, b
		-- from -1 to 1)
		local list = {}
		for a = -1, 1 do
			for b = -1, 1 do
				list[#list + 1] = c + (a + b) * ROW + (a - b)
			end
		end
		local patch = gen.cells(list)
		if not inChamber or patch:has(land) or not open:has(land) then
			return false
		end
		local cells = patch:list()
		for _, t in cells do
			if not unusedAround(t) then
				return false
			end
		end
		reachedAll = reachedAll or reached(nil)
		-- (With every patch before it given way too: each one's cells all reached.)
		local gone = table.clone(sprung)
		for _, t in cells do
			gone[#gone + 1] = t
		end
		if reached(gone) ~= reachedAll - #gone then
			return false -- (it would cut something off)
		end
		sprung = gone
		local lx, ly = util.xy(land)
		local hole = { fallX = lx * 23 + 11, fallY = ly * 23 + 11, timeout = 4 } -- (as G_Forest's: it gives way at once)
		for _, t in cells do
			local x, y = at(t)
			w:thing("PitCrumblingFloor", x, y, { enabled = false, hole = hole, status = 2 })
			claim(t, 1)
		end
		return true
	end

	-- Arrow traps on the back walls beside the corridor ahead, facing across it: a north-east way has
	-- "/" walls on its north-west side, their floor south-east (an ArrowTrap1's facing); a
	-- south-east way "\" walls, floor south-west (ArrowTrap2's). Up to three walls, two apart, their
	-- fronts within three of the route four to six cells on.
	local function placeArrows(k: number, d: number): boolean
		local shape = if SIDE_DX[d] == -SIDE_DY[d] then 0 else 1
		local mounts = site.backs:mounts(site.walls, shape, open, used, path, k + 2, k + 6, 3, 2, 3)
		if #mounts < 2 then
			return false
		end
		local name = nameOf("Arrows")
		local traps, plates = {}, {}
		local fronts = {}
		for _, i in mounts do
			local wc, front = site.backs:at(i)
			local x, y = onWall(wc, front)
			traps[#traps + 1] = w:thing(if shape == 0 then "ArrowTrap1" else "ArrowTrap2", x, y,
				{ name = name .. "Trap" .. tostring(#traps), enabled = false })
			used:add(front)
			fronts[#fronts + 1] = { wc, front }
		end
		-- The triggers in their lanes: each trap's cells straight out across the corridor (where it
		-- sees, in front of it), eight at most (well inside its 350 square).
		local lanes = gen.cells()
		for _, m in fronts do
			local f = m[2] - m[1]
			local c = m[2]
			local n = 0
			while n < 8 and open:has(c) do
				lanes:add(c)
				n += 1
				c += f
			end
		end
		for _, c in lanes:list() do
			local x, y = at(c)
			plates[#plates + 1] = w:thing("Trigger", x, y, { name = name .. "Plate" .. tostring(#plates), trigger = plate(30, name .. "Fire") })
			used:add(c)
		end
		w:objectGroup(name .. "Traps", traps)
		w:objectGroup(name .. "Plates", plates)
		-- On for a second and a half (each shoots on sight, then once a second), the triggers back
		-- after four.
		script:fn(name .. "Fire")
			:pushString(name .. "Plates"):call("ObjectGroup"):call("ObjectGroupOff")
			:pushString(name .. "Traps"):call("ObjectGroup"):call("ObjectGroupOn")
			:pushInt(45):pushFunction(name .. "Off"):call("FrameTimer")
			:pushInt(120):pushFunction(name .. "Rearm"):call("FrameTimer")
		script:fn(name .. "Off"):pushString(name .. "Traps"):call("ObjectGroup"):call("ObjectGroupOff")
		script:fn(name .. "Rearm"):pushString(name .. "Plates"):call("ObjectGroup"):call("ObjectGroupOn")
		return true
	end

	-- A SpikeBlock shuttling across the corridor: the line's cells in order along it; the block
	-- shuttles between the second and the last but one (diagonal, as every shipped mover's path: a
	-- straight one along an axis never sees its waypoint passed).
	local function placeCrusher(c: number, d: number, line): boolean
		local ex, ey = SIDE_DX[d], -SIDE_DY[d]
		local cx, cy = util.xy(c)
		local across = table.clone(line)
		local function along(t)
			local tx, ty = util.xy(t)
			return (tx - cx) * ex + (ty - cy) * ey
		end
		table.sort(across, function(p, q)
			return along(p) < along(q)
		end)
		if #across < 5 then
			return false
		end
		local a, b = across[2], across[#across - 1]
		local name = nameOf("Crusher")
		local ax, ay = at(a)
		local bx, by = at(b)
		local block = w:thing("SpikeBlock", ax, ay, { name = name })
		w:thing("Mover", ax, ay, { enabled = false, mover = { speed = 10, extent = block, base = 1.875 } }) -- (G_Temple's SpikeBlock02-04: about 55 a second)
		local wa = w:waypoint(name .. "A", ax, ay)
		local wb = w:waypoint(name .. "B", bx, by)
		w:link(wa, wb)
		script:fn("MapInitialize" .. name):object(name):pushString(name .. "B"):call("Waypoint"):call("Move")
		for _, t in line do
			claim(t, 1)
		end
		return true
	end

	-- A fist trap, cast by an ImaginaryCaster: its power 1, a small fist (FistOfVengeanceDamage's
	-- 50). The trigger itself would cast at 3, a large fist's 200, more than a fresh warrior's whole
	-- health (G_Crypts' fist traps do: their PressurePlates cast; no shipped map holds an
	-- ImaginaryCaster). Milder by choice.
	local function placeFist(c: number)
		local fn = nameOf("Fist")
		local x, y = at(c)
		w:thing("Trigger", x, y, { name = fn, trigger = plate(40, fn) })
		w:thing("ImaginaryCaster", x, y, { name = fn .. "Caster" })
		claim(c, 2)
		script:fn(fn)
			:pushInt(SELF):call("ObjectOff")
			:pushString("SPELL_FIST"):object(fn .. "Caster")
			:call("GetCaller"):call("GetObjectX")
			:call("GetCaller"):call("GetObjectY")
			:call("CastSpellObjectLocation")
			:pushInt(90):call("GetTrigger"):pushFunction(rearm()):call("FrameTimerWithArg")
	end

	-- A trap across the corridor going `d` at route cell `k` (`c`): a free line across it, the cells
	-- either side of it free too. False where it can't go.
	local function placeLineTrap(kind: number, k: number, c: number, d: number): boolean
		local line = lineAcross(open, c, d)
		if #line == 0 or 7 < #line then
			return false
		end
		for _, t in line do
			if not (unused(t) and unused(t + d) and unused(t - d)) then
				return false
			end
		end
		if kind == SPIKES then
			for _, t in line do
				local x, y = at(t)
				w:thing("PeriodicSpike", x, y, {})
				claim(t, 1)
			end
			return true
		elseif kind == GLYPH then
			local spell = if site.depth < 3 then MILD[rng:next() % 3 + 1] else HARD[rng:next() % 3 + 1]
			local x, y = at(c)
			w:thing("Glyph", x, y, { glyph = { x = x, y = y, spell = spell } })
			claim(c, 2)
			return true
		elseif kind == ARROWS then
			return placeArrows(k, d)
		elseif kind == CRUSHER then
			return placeCrusher(c, d, line)
		end
		placeFist(c) -- (a fist; a crumble goes by placeCrumble: never here)
		return true
	end

	-- Traps along the route, away from the start and the end.
	local function placeTraps()
		local spots = {}
		for k = 1, #path - 6 do
			if chebyshev(path[k], start) >= 14 and chebyshev(path[k], finish) >= 10 then
				spots[#spots + 1] = k
			end
		end
		-- (The order the random pick indexes.)
		local kinds = { SPIKES, GLYPH, ARROWS }
		if site.depth >= 2 then
			kinds[#kinds + 1] = FIST
			kinds[#kinds + 1] = CRUMBLE
		end
		if site.depth >= 3 then
			kinds[#kinds + 1] = CRUSHER
		end
		local t = 0
		while t < site.traps and #spots > 0 do
			for _ = 1, 40 do -- (until one is placed)
				local k = spots[rng:next() % #spots + 1]
				local c = path[k]
				local d = path[k + 1] - c
				if isStep(d) then
					local kind = kinds[rng:next() % #kinds + 1]
					local done
					if kind == CRUMBLE then
						done = placeCrumble(k, c)
					else
						done = placeLineTrap(kind, k, c, d)
					end
					if done then
						note(TRAP_NAMES[kind])
						break -- (placed: the next trap)
					end
				end
			end
			t += 1
		end
	end

	-- Deep down, fire skulls on a chamber's back walls (always on): one from depth 4, two from 8
	-- (each shoots once a second while it sees someone).
	local function placeSkulls()
		local chambers = lay.chambers
		if site.depth < 4 or #chambers <= 2 then
			return
		end
		-- One chamber of the route, not the start nor the end.
		local room = chambers[2 + rng:next() % (#chambers - 2)]
		local skulls = if site.depth >= 8 then 2 else 1
		local count = 0
		for _, i in site.backs:near(centre(room), room.radius) do
			if count == skulls then
				break
			end
			local wc, front = site.backs:at(i)
			if unused(front) then
				local slash = site.walls:get(wc) == 0
				local x, y = onWall(wc, front)
				w:thing(if slash then "Skull4" else "Skull3", x, y, {}) -- (SE / SW, into the room)
				claim(front, 2)
				count += 1
			end
		end
		if count > 0 then
			note("skulls")
		end
	end

	for _, v in state.vaults do
		placeVault(v)
	end
	placeObelisks()
	for _, kd in state.doors do
		placeDoor(kd)
	end
	for _, a in state.ambushes do
		placeAmbush(a)
	end
	placeTraps()
	placeSkulls()
	return { monsters = monsters, placed = placed }
end

return features
