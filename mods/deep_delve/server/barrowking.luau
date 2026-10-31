--!strict
-- Act 2's boss, the barrow-king (data/monsters.luau's barrowking; his numbers numbers.barrowKing),
-- on his mound: a ring of graves round the arena (the generator's lair look "barrow", its spots'
-- `graves`). boss.luau makes him, shows his bar and takes his death; this is his fight, thought
-- over on boss.luau's beat (one timer, one monster; a second one, a frame long, only while he
-- charges).
--
--   asleep   until a player comes within `wake` of him: he speaks, and the fight begins.
--   1        the graves give up his dead: every so often a grave sparks and a moment later one of
--            his first list (skeletons, zombies) climbs out of it, a few graves at a time round
--            the arena (never at his side), at most so many standing at once. And his charge: a
--            player some way off is marked by sparks along the ground between them; a moment later
--            he rushes along that line, striking and throwing back whoever he passes, until a wall
--            or his reach stops him (step aside from the sparks and he misses).
--   2        below the mist's share of his health: the grave-mist (the game's Toxic Cloud, his own)
--            rises under a player's feet a moment after a smoke warning there; the graves now give
--            up ghosts and vile zombies too.
--   3        enraged below its share: hasted, the barrow quakes once (an explosion round him and a
--            blow to everyone near), his charge comes more often, more of his dead at a time, a
--            second mist, and the sky's fist (the game's Fist of Vengeance) falls on a player
--            every few seconds.
--
-- Whatever is his (the mist, the fists) is scaled as his own hits are, times his `spells`
-- (combat.luau); none of it hurts the mod's monsters (combat.luau: a monster's hits on a monster
-- are none).

local monsters = require("monsters")
local numbers = require("data/numbers")

local king = {}

local PLAYERS: any = { players = true, alive = true }

local MSG_YELLOW_SPARKS = 0x82
local MSG_CYAN_SPARKS = 0x83
local MSG_EXPLOSION = 0x85
local MSG_SMOKE_BLAST = 0x8A
local MSG_TURN_UNDEAD = 0xA0

-- How near a player wakes him.
local WAKE = 320

-- The graves his dead climb from: the generator's (a barrow arena's), else a ring round his spot.
local function gravesOf(spots: any): { any }
	if type(spots.graves) == "table" and #spots.graves > 0 then
		return spots.graves
	end
	local out = {}
	local b = spots.boss
	for k = 0, 5 do
		local a = k * math.pi / 3
		local x, y = nox.clearPointNear(b.x + 170 * math.cos(a), b.y + 170 * math.sin(a), 40)
		table.insert(out, { x = x, y = y })
	end
	return out
end

-- Whether the fight `c` is still on (he lives, his map is the one being played).
local function on(c: any): boolean
	return not c.over and nox.living(c.id)
end

-- The players within `far` of (x, y) and not within `near`, alive.
local function playersAround(x: number, y: number, near: number, far: number): { number }
	local out = {}
	for _, u in nox.unitsNear(x, y, far, PLAYERS) do
		local ux, uy = nox.position(u)
		local dx, dy = ux - x, uy - y
		if dx * dx + dy * dy >= near * near then
			table.insert(out, u)
		end
	end
	return out
end

-- ---- His dead from the graves

local function raise(c: any)
	local k = c.k
	local n = numbers.barrowKing.graves
	local standing = nox.keepLiving(k.adds)
	local want = math.min(n.count + (if k.enraged then n.enraged else 0), n.most - standing - k.rising)
	if want <= 0 then
		return
	end
	local list: { string } = if k.misted and c.def.laterAdds then c.def.laterAdds else c.def.adds
	nox.unitSay(c.id, c.def.lines.graves, 75)
	for _ = 1, want do
		local g = k.graves[k.nextGrave]
		k.nextGrave = k.nextGrave % #k.graves + 1
		nox.effect(MSG_TURN_UNDEAD, g.x, g.y)
		nox.effect(MSG_CYAN_SPARKS, g.x, g.y)
		k.rising += 1
		local t = list[nox.random(#list)]
		nox.after(n.warn, function()
			k.rising -= 1
			if not on(c) then
				return
			end
			local id = monsters.spawn(t, g.x, g.y, n.tier, c.info.level)
			if id then
				table.insert(k.adds, id)
				k.raised += 1
				local ax, ay = nox.position(id)
				nox.effect(MSG_SMOKE_BLAST, ax, ay)
			end
		end)
	end
end

-- ---- His charge

local function rushStep(c: any)
	local k = c.k
	local r = k.rush
	if not r then
		return
	end
	local n = numbers.barrowKing.charge
	local x, y = nox.position(c.id)
	local nx, ny = x + r.dx * n.speed, y + r.dy * n.speed
	r.left -= 1
	if not on(c) or r.left < 0 or not nox.traceRay(x, y, nx, ny, 1) then
		nox.cancelTimer(r.timer)
		k.rush = nil
		return
	end
	nox.setPosition(c.id, nx, ny)
	for _, u in nox.unitsNear(nx, ny, n.reach, PLAYERS) do
		if not r.hit[u] then
			r.hit[u] = true
			k.charged += 1
			nox.damage(u, c.id, n.damage, 2) -- (crushing; combat.luau scales it as his hits)
			nox.applyForce(u, nx, ny, n.force)
			nox.effect(MSG_YELLOW_SPARKS, nx, ny)
		end
	end
end

-- A player marked for his charge: sparks along the line between them, the rush a moment later
-- along that line (where the player stood at the warning). Whether one was marked.
local function charge(c: any): boolean
	local k = c.k
	local n = numbers.barrowKing.charge
	local x, y = nox.position(c.id)
	local around = playersAround(x, y, n.near, n.far)
	local seen = {}
	for _, u in around do
		local ux, uy = nox.position(u)
		if nox.traceRay(x, y, ux, uy, 1) then
			table.insert(seen, u)
		end
	end
	if #seen == 0 then
		return false
	end
	local u = seen[nox.random(#seen)]
	local tx, ty = nox.position(u)
	local dx, dy = tx - x, ty - y
	local len = math.max(1, math.sqrt(dx * dx + dy * dy))
	dx, dy = dx / len, dy / len
	nox.unitSay(c.id, c.def.lines.charge, 60)
	local function mark()
		for s = 1, 4 do
			local d = len * s / 4
			nox.effect(MSG_YELLOW_SPARKS, x + dx * d, y + dy * d)
		end
	end
	mark()
	nox.after(n.warn // 2, function()
		if on(c) then
			mark()
		end
	end)
	-- (its length: to the player's spot and a little past it, at most his reach)
	local frames = math.min(n.frames, math.ceil((len + 40) / n.speed))
	k.marked = { x = tx, y = ty }
	nox.after(n.warn, function()
		if not on(c) or k.rush then
			return
		end
		k.charges += 1
		k.rush = { dx = dx, dy = dy, left = frames, hit = {} }
		k.rush.timer = nox.every(1, rushStep, c)
	end)
	return true
end

-- ---- The grave-mist and the sky's fist

local function mist(c: any)
	local k = c.k
	local n = numbers.barrowKing.mist
	local x, y = nox.position(c.id)
	local around = playersAround(x, y, 0, 600)
	local count = math.min(#around, n.count + (if k.enraged then n.enraged else 0))
	if count == 0 then
		return
	end
	nox.unitSay(c.id, c.def.lines.mist, 60)
	for _ = 1, count do
		local u = table.remove(around, nox.random(#around)) :: number
		local px, py = nox.position(u)
		nox.effect(MSG_SMOKE_BLAST, px, py)
		nox.after(n.warn, function()
			if on(c) then
				local bx, by = nox.position(c.id)
				-- (the game's own cast, his: it needs a clear line from him to the spot)
				if nox.cast("SPELL_TOXIC_CLOUD", c.id, nil, px, py, 0) then
					k.mists += 1
				else
					-- (no line: the mist rises where it can, half way)
					if nox.cast("SPELL_TOXIC_CLOUD", c.id, nil, (bx + px) / 2, (by + py) / 2, 0) then
						k.mists += 1
					end
				end
			end
		end)
	end
end

local function fist(c: any)
	local k = c.k
	local x, y = nox.position(c.id)
	local around = playersAround(x, y, 0, 600)
	if #around == 0 then
		return
	end
	local u = around[nox.random(#around)]
	local px, py = nox.position(u)
	if nox.cast("SPELL_FIST", c.id, nil, px, py, numbers.barrowKing.enrage.fistPower) then
		k.fists += 1
	end
end

-- The quake as he's enraged: an explosion and smoke round him, and the game's own Earthquake, his
-- (the ground shakes on every client near; it strikes whoever stands round him, less further off,
-- scaled as his own hits are; his dead are spared, combat.luau).
local function quake(c: any)
	local x, y = nox.position(c.id)
	nox.effect(MSG_EXPLOSION, x, y)
	for k = 0, 5 do
		local a = k * math.pi / 3
		nox.effect(MSG_SMOKE_BLAST, x + 90 * math.cos(a), y + 90 * math.sin(a))
	end
	nox.cast("SPELL_EARTHQUAKE", c.id, nil, x, y, numbers.barrowKing.enrage.quake)
end

-- ---- The fight

-- His fight begun (`c`: boss.luau's, his id, def and area info; `spots`: the lair's).
function king.start(c: any, spots: any)
	c.k = {
		awake = false,
		phase = 0,
		misted = false,
		enraged = false,
		adds = {},
		rising = 0,
		raised = 0,
		charges = 0,
		charged = 0,
		mists = 0,
		fists = 0,
		graves = gravesOf(spots),
		nextGrave = 1,
		gravesAt = 0,
		chargeAt = 0,
		mistAt = 0,
		fistAt = 0,
		rush = nil,
		marked = nil,
	}
	c.k.nextGrave = nox.random(#c.k.graves)
end

-- His fight on the beat.
function king.think(c: any)
	local k = c.k
	if not k or not on(c) then
		return
	end
	local n = numbers.barrowKing
	local o = nox.object(c.id)
	if not o then
		return
	end
	local now = nox.frame()
	if not k.awake then
		if nox.countNear(o.x, o.y, WAKE, PLAYERS) == 0 then
			return
		end
		k.awake = true
		k.phase = 1
		nox.unitSay(c.id, c.def.lines.wake, 90)
		k.gravesAt = now + n.graves.every // 3
		k.chargeAt = now + n.charge.every // 2
	end
	local share = o.health / math.max(1, o.maxHealth)
	if not k.misted and share <= n.mist.at then
		k.misted = true
		k.phase = 2
		k.mistAt = now
	end
	if not k.enraged and share <= n.enrage.at then
		k.enraged = true
		k.phase = 3
		nox.enchant(c.id, c.def.enrage, 0)
		nox.unitSay(c.id, c.def.lines.enrage, 90)
		quake(c)
		k.fistAt = now + n.enrage.fistEvery // 2
		k.chargeAt = math.min(k.chargeAt, now + n.charge.enragedEvery)
	end
	-- (mid-charge: nothing else)
	if k.rush then
		return
	end
	if now >= k.gravesAt then
		k.gravesAt = now + n.graves.every
		raise(c)
	end
	if now >= k.chargeAt then
		k.chargeAt = now + (if charge(c) then (if k.enraged then n.charge.enragedEvery else n.charge.every) else 30)
	end
	if k.misted and now >= k.mistAt then
		k.mistAt = now + n.mist.every
		mist(c)
	end
	if k.enraged and now >= k.fistAt then
		k.fistAt = now + n.enrage.fistEvery
		fist(c)
	end
end

-- His fight over (his death, or his map gone): nothing more of it runs.
function king.stop(c: any)
	c.over = true
	local k = c.k
	if k and k.rush then
		nox.cancelTimer(k.rush.timer)
		k.rush = nil
	end
end

-- What "dd fight" says: "phase P awake|asleep, adds standing/raised, charges N (hits H), mists M,
-- fists F, graves G".
function king.line(c: any): string
	local k = c.k
	if not k then
		return "no fight"
	end
	local standing: number = nox.keepLiving(k.adds)
	local awake: string = if k.awake then "awake" else "asleep"
	return "phase " .. tostring(k.phase) .. " " .. awake .. ", adds " .. tostring(standing) .. "/" .. tostring(k.raised) .. ", charges "
		.. tostring(k.charges) .. " (hits " .. tostring(k.charged) .. "), mists " .. tostring(k.mists) .. ", fists " .. tostring(k.fists)
		.. ", graves " .. tostring(#k.graves)
end

-- The fight's next blows brought forward, for "dd fight now": each comes on the next beat.
function king.hurry(c: any)
	local k = c.k
	if k then
		k.gravesAt, k.chargeAt, k.mistAt, k.fistAt = 0, 0, 0, 0
	end
end

return king
