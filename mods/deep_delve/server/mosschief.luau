--!strict
-- Act 3's boss, the troll chieftain (data/monsters.luau's mosschief; his numbers
-- numbers.mossChief), in his glade: fire pits at the arena's quarters, his camp's skull posts and
-- logs round its edge (the generator's lair look "glade", its spots' `fires` and `kin`). boss.luau
-- makes him, shows his bar and takes his death; this is his fight, thought over on boss.luau's beat
-- (one timer, one monster).
--
--   asleep   until a player comes within `wake` of him: he speaks, and the fight begins.
--   1        his wounds close: a share of his most health a second while he's hurt, unless fire
--            touched him a moment ago (a flame hit from anything, his own fire pits too, or
--            standing by one of them). He hurls a stone at whoever keeps away from him, and his
--            kin (wolves) run in at the glade's way in, behind the party, a few at a time.
--   2        below the roots' share: the wood's Force of Nature (the game's, his) rolls at a
--            player a moment after green sparks at its feet, he hauls the last one who burnt him
--            (else the one furthest off) to his side, and his kin are bears and trolls now.
--   3        enraged below its share: hasted for good, his wounds close twice as fast, he stomps
--            the ground round him (a ring of sparks, then a blow that throws back whoever stands
--            in it) and his stones come three at a time.
--
-- So fire pays: a wizard's flames keep his wounds open, and anyone can lead him by his own fire
-- pits. The fire is heard by combat.luau (a flame hit on him marks him `burntAt`, and `burntBy`
-- the hero behind it); the pits are a look at four points on the beat. Whatever is his (the Force
-- of Nature) is scaled as his own hits are, times his `spells`; his stones are his own blows. None
-- of it hurts the mod's monsters (combat.luau).

local monsters = require("monsters")
local numbers = require("data/numbers")

local chief = {}

local PLAYERS: any = { players = true, alive = true }

local MSG_YELLOW_SPARKS = 0x82
local MSG_CYAN_SPARKS = 0x83
local MSG_LESSER_EXPLOSION = 0x86
local MSG_SMOKE_BLAST = 0x8A

-- The damage kind of a flame (the game's damage types: fire is 1).
local FLAME = 1

-- Whether the fight `c` is still on (he lives, his map is the one being played).
local function on(c: any): boolean
	return not c.over and nox.living(c.id)
end

-- The points his kin run in at: the generator's (the glade's way in), else a few round his spot.
local function kinOf(spots: any): { any }
	if type(spots.kin) == "table" and #spots.kin > 0 then
		return spots.kin
	end
	local out = {}
	local b = spots.boss
	for k = 0, 2 do
		local a = k * 2 * math.pi / 3
		local x, y = nox.clearPointNear(b.x + 200 * math.cos(a), b.y + 200 * math.sin(a), 40)
		table.insert(out, { x = x, y = y })
	end
	return out
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

-- ---- Fire and mending

-- Whether he's scorched now: a flame hit a moment ago (combat.luau's `burntAt` on his entry), or
-- standing by one of the glade's fire pits (which scorches him again).
local function scorched(c: any, now: number): boolean
	local k = c.k
	local n = numbers.mossChief.mend
	local entry = monsters.info(c.id)
	local burnt: number = if entry and type(entry.burntAt) == "number" then entry.burntAt else -1e9
	local x, y = nox.position(c.id)
	for _, f in k.fires do
		local dx, dy = f.x - x, f.y - y
		if dx * dx + dy * dy <= n.fireReach * n.fireReach then
			burnt = now
			if entry then
				entry.burntAt = now
			end
			break
		end
	end
	if burnt > k.lastBurnt then
		-- (a new scorch: his cry, at most so often; a puff of smoke on him)
		if burnt - k.lastBurnt > n.scorch and now - k.saidBurnt >= n.say then
			k.saidBurnt = now
			nox.unitSay(c.id, c.def.lines.burnt, 60)
		end
		if burnt - k.lastBurnt > n.scorch then
			k.scorches += 1
			nox.effect(MSG_SMOKE_BLAST, x, y)
		end
		k.lastBurnt = burnt
	end
	return now - burnt < n.scorch
end

-- His wounds closing on the beat (none while scorched).
local function mend(c: any, o: any, now: number)
	local k = c.k
	local n = numbers.mossChief.mend
	k.scorched = scorched(c, now)
	if k.scorched or o.health >= o.maxHealth then
		return
	end
	local share = if k.enraged then n.enraged else n.share
	local amount = math.max(1, math.floor(o.maxHealth * share * numbers.mossChief.beat / 30 + 0.5))
	nox.heal(c.id, amount)
	k.mended += amount
	if not k.saidMend then
		k.saidMend = true
		nox.unitSay(c.id, c.def.lines.mend, 75)
	end
end

-- ---- His kin from the way in

local function kin(c: any)
	local k = c.k
	local n = numbers.mossChief.kin
	local standing = nox.keepLiving(k.adds)
	local want = math.min(n.count + (if k.enraged then n.enraged else 0), n.most - standing - k.coming)
	if want <= 0 then
		return
	end
	local list: { string } = if k.rooted and c.def.laterAdds then c.def.laterAdds else c.def.adds
	nox.unitSay(c.id, c.def.lines.kin, 75)
	for _ = 1, want do
		local p = k.kin[k.nextKin]
		k.nextKin = k.nextKin % #k.kin + 1
		nox.effect(MSG_SMOKE_BLAST, p.x, p.y)
		k.coming += 1
		local t = list[nox.random(#list)]
		nox.after(n.warn, function()
			k.coming -= 1
			if not on(c) then
				return
			end
			local id = monsters.spawn(t, p.x, p.y, n.tier, c.info.level)
			if id then
				table.insert(k.adds, id)
				k.called += 1
			end
		end)
	end
end

-- ---- His stones

-- A stone (or a fan of them, enraged) at a player some way off with a clear line; whether one flew.
local function hurl(c: any): boolean
	local k = c.k
	local n = numbers.mossChief.hurl
	local x, y = nox.position(c.id)
	local seen = {}
	for _, u in playersAround(x, y, n.near, n.far) do
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
	local dir = nox.directionOf(tx - x, ty - y)
	nox.direction(c.id, dir)
	nox.unitSay(c.id, c.def.lines.hurl, 45)
	local count = if k.enraged then n.fan else 1
	local flew = false
	for i = 1, count do
		local off = (i - 1 - (count - 1) / 2) * n.spread
		local d = math.floor(dir + off + 0.5) % 256
		local stone = nox.shoot("ThrowingStone", c.id, d)
		if stone then
			nox.setDrawScale(stone, n.scale)
			nox.setHitRadius(stone, n.hit)
			k.stones += 1
			flew = true
		end
	end
	return flew
end

-- ---- The roots (phase 2 on)

local function roots(c: any)
	local k = c.k
	local n = numbers.mossChief.roots
	local x, y = nox.position(c.id)
	local around = playersAround(x, y, 0, 600)
	if #around == 0 then
		return
	end
	local u = around[nox.random(#around)]
	local px, py = nox.position(u)
	nox.unitSay(c.id, c.def.lines.roots, 60)
	nox.effect(MSG_CYAN_SPARKS, px, py)
	nox.after(n.warn, function()
		if on(c) and nox.cast("SPELL_FORCE_OF_NATURE", c.id, nil, px, py, n.power) then
			k.roots += 1
		end
	end)
end

-- The last one who burnt him (alive, within reach), else the player furthest off, hauled to him.
local function pull(c: any)
	local k = c.k
	local n = numbers.mossChief.pull
	local x, y = nox.position(c.id)
	local entry = monsters.info(c.id)
	local target: number? = nil
	local by: number? = entry and entry.burntBy
	if by and nox.living(by) then
		local bx, by2 = nox.position(by)
		local dx, dy = bx - x, by2 - y
		if dx * dx + dy * dy <= n.far * n.far then
			target = by
		end
	end
	if not target then
		local best = -1
		for _, u in playersAround(x, y, 0, n.far) do
			local ux, uy = nox.position(u)
			local d = (ux - x) * (ux - x) + (uy - y) * (uy - y)
			if d > best then
				best, target = d, u
			end
		end
	end
	if not target then
		return
	end
	nox.unitSay(c.id, c.def.lines.pull, 60)
	local tx, ty = nox.position(target)
	nox.effect(MSG_CYAN_SPARKS, tx, ty)
	nox.applyForce(target, x, y, -n.force)
	k.pulls += 1
end

-- ---- The stomp (enraged)

-- A ring of sparks round him, then the blow round where he stood; whether one began.
local function stomp(c: any): boolean
	local k = c.k
	local n = numbers.mossChief.stomp
	local x, y = nox.position(c.id)
	if nox.countNear(x, y, n.range, PLAYERS) == 0 then
		return false
	end
	nox.unitSay(c.id, c.def.lines.stomp, 45)
	for i = 0, 7 do
		local a = i * math.pi / 4
		nox.effect(MSG_YELLOW_SPARKS, x + n.reach * 0.8 * math.cos(a), y + n.reach * 0.8 * math.sin(a))
	end
	nox.after(n.warn, function()
		if not on(c) then
			return
		end
		local sx, sy = nox.position(c.id)
		nox.effect(MSG_LESSER_EXPLOSION, sx, sy)
		for _, u in nox.unitsNear(sx, sy, n.reach, PLAYERS) do
			nox.applyForce(u, sx, sy, n.force)
		end
		-- (crushing, his own blow: combat.luau scales it as his hits; he and his kin spared)
		nox.damageAt(sx, sy, n.reach, n.full, n.damage, 2, c.id, nil, true)
		k.stomps += 1
	end)
	return true
end

-- ---- The fight

-- His fight begun (`c`: boss.luau's, his id, def and area info; `spots`: the lair's). His entry is
-- marked for combat.luau to note the fire that touches him.
function chief.start(c: any, spots: any)
	c.k = {
		awake = false,
		phase = 0,
		rooted = false,
		enraged = false,
		scorched = false,
		saidMend = false,
		adds = {},
		coming = 0,
		called = 0,
		mended = 0,
		scorches = 0,
		lastBurnt = -1e9,
		saidBurnt = -1e9,
		stones = 0,
		roots = 0,
		pulls = 0,
		stomps = 0,
		kin = kinOf(spots),
		nextKin = 1,
		fires = if type(spots.fires) == "table" then spots.fires else {},
		kinAt = 0,
		hurlAt = 0,
		rootsAt = 0,
		pullAt = 0,
		stompAt = 0,
	}
	local entry = monsters.info(c.id)
	if entry then
		entry.scorchable = true
	end
end

-- His fight on the beat.
function chief.think(c: any)
	local k = c.k
	if not k or not on(c) then
		return
	end
	local n = numbers.mossChief
	local o = nox.object(c.id)
	if not o then
		return
	end
	local now = nox.frame()
	if not k.awake then
		if nox.countNear(o.x, o.y, n.wake, PLAYERS) == 0 then
			return
		end
		k.awake = true
		k.phase = 1
		nox.unitSay(c.id, c.def.lines.wake, 90)
		k.kinAt = now + n.kin.every // 3
		k.hurlAt = now + n.hurl.every // 2
	end
	mend(c, o, now)
	local share = o.health / math.max(1, o.maxHealth)
	if not k.rooted and share <= n.roots.at then
		k.rooted = true
		k.phase = 2
		k.rootsAt = now
		k.pullAt = now + n.pull.every // 2
	end
	if not k.enraged and share <= n.enrage.at then
		k.enraged = true
		k.phase = 3
		nox.enchant(c.id, c.def.enrage, 0)
		nox.unitSay(c.id, c.def.lines.enrage, 90)
		k.stompAt = now
		k.hurlAt = math.min(k.hurlAt, now + n.hurl.enragedEvery)
	end
	if now >= k.kinAt then
		k.kinAt = now + n.kin.every
		kin(c)
	end
	if now >= k.hurlAt then
		k.hurlAt = now + (if hurl(c) then (if k.enraged then n.hurl.enragedEvery else n.hurl.every) else 30)
	end
	if k.rooted and now >= k.rootsAt then
		k.rootsAt = now + n.roots.every
		roots(c)
	end
	if k.rooted and now >= k.pullAt then
		k.pullAt = now + n.pull.every
		pull(c)
	end
	if k.enraged and now >= k.stompAt then
		k.stompAt = now + (if stomp(c) then n.stomp.every else 30)
	end
end

-- His fight over (his death, or his map gone): nothing more of it runs.
function chief.stop(c: any)
	c.over = true
end

-- What "dd fight" says: "phase P awake|asleep, scorched|mending, mended M, scorches S, adds
-- standing/called, stones N, roots R, pulls P, stomps S, fires F".
function chief.line(c: any): string
	local k = c.k
	if not k then
		return "no fight"
	end
	local standing: number = nox.keepLiving(k.adds)
	local awake: string = if k.awake then "awake" else "asleep"
	local state: string = if k.scorched then "scorched" else "mending"
	return "phase " .. tostring(k.phase) .. " " .. awake .. ", " .. state .. ", mended " .. tostring(k.mended) .. ", scorches " .. tostring(k.scorches)
		.. ", adds " .. tostring(standing) .. "/" .. tostring(k.called) .. ", stones " .. tostring(k.stones) .. ", roots " .. tostring(k.roots)
		.. ", pulls " .. tostring(k.pulls) .. ", stomps " .. tostring(k.stomps) .. ", fires " .. tostring(#k.fires)
end

-- The fight's next blows brought forward, for "dd fight now": each comes on the next beat.
function chief.hurry(c: any)
	local k = c.k
	if k then
		k.kinAt, k.hurlAt, k.rootsAt, k.pullAt, k.stompAt = 0, 0, 0, 0, 0
	end
end

-- A flame hit on one of the mod's monsters (combat.luau): noted on its entry when it's his
-- (`by`: the hero behind the hit, or nil: a fire pit).
function chief.burnt(entry: any, by: number?)
	if entry and entry.scorchable then
		entry.burntAt = nox.frame()
		if by then
			entry.burntBy = by
		end
	end
end

chief.FLAME = FLAME

return chief
