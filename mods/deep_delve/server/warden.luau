--!strict
-- Act 4's boss, the ruin-warden (data/monsters.luau's warden; his numbers numbers.ruinWarden), in
-- the ruined court: braziers at the arena's quarters, the court's statues and broken columns round
-- its edge (the generator's lair look "court", its spots' `ring`). boss.luau makes him, shows his
-- bar and takes his death; this is his fight, thought over on boss.luau's beat (one timer, one
-- monster).
--
--   asleep   until a player comes within `wake` of him: he speaks, and the fight begins.
--   1        plated: while any of his stone plates holds he takes only a share of every blow. He
--            pounds the ground where a player stands (a ring of sparks there, a moment later the
--            ground heaves: whoever is still in it is struck and thrown back). A pound that strikes
--            nobody shakes a plate off him: he reels (held a moment) and takes more of every blow
--            while he does. The court's statues and columns flare and give up its wardens (flying
--            golems), a few at a time.
--   2        bare: his last plate gone (or below his health's share where every plate left falls):
--            the court quakes (the game's Earthquake, his), and a sand-devil is loosed at the
--            arena's middle every so often, drifting after the nearest player, slowing and scouring
--            whoever it catches. A pound that strikes nobody still leaves him reeling and open. The
--            wardens now are flying golems and skeleton lords.
--   3        enraged below its share: hasted for good, his pound more often, the sand-devils
--            sooner, and the court's masonry falls round the party (sparks, then the stones).
--
-- So moving pays: whoever steps out of the sparks makes him miss, and a miss is what opens him up.
-- His plates are a share on his monster entry (`taken`) that combat.luau reads for every blow the
-- party lands; nothing walks the world but his beat's looks round him, the sand-devil and the
-- players he picks.

local monsters = require("monsters")
local numbers = require("data/numbers")

local warden = {}

local PLAYERS: any = { players = true, alive = true }

local MSG_YELLOW_SPARKS = 0x82
local MSG_CYAN_SPARKS = 0x83
local MSG_EXPLOSION = 0x85
local MSG_LESSER_EXPLOSION = 0x86
local MSG_SMOKE_BLAST = 0x8A

-- A crushing blow's damage kind.
local CRUSH = 2

-- Whether the fight `c` is still on (he lives, his map is the one being played).
local function on(c: any): boolean
	return not c.over and nox.living(c.id)
end

-- The pieces the wardens rise at: the generator's (the court's statues and columns), else a ring
-- round his spot.
local function ringOf(spots: any): { any }
	if type(spots.ring) == "table" and #spots.ring > 0 then
		return spots.ring
	end
	local out = {}
	local b = spots.boss
	for k = 0, 5 do
		local a = k * math.pi / 3
		local x, y = nox.clearPointNear(b.x + 180 * math.cos(a), b.y + 180 * math.sin(a), 40)
		table.insert(out, { x = x, y = y })
	end
	return out
end

-- The players within `far` of (x, y), alive.
local function playersNear(x: number, y: number, far: number): { number }
	return nox.unitsNear(x, y, far, PLAYERS)
end

-- ---- His plates

-- How much of a blow from the party reaches him now (his monster entry's `taken`, combat.luau's).
local function setTaken(c: any, now: number)
	local k = c.k
	local n = numbers.ruinWarden.plates
	local entry = monsters.info(c.id)
	if not entry then
		return
	end
	if now < k.exposedUntil then
		entry.taken = n.exposed
	elseif k.plates > 0 then
		entry.taken = n.taken
	else
		entry.taken = nil
	end
end

-- His draw scale for the plates he has left.
local function redraw(c: any)
	local k = c.k
	local n = numbers.ruinWarden.plates
	local draw: number = tonumber(c.def.draw) or 1
	nox.setDrawScale(c.id, math.max(0.5, draw - n.shrink * (n.count - k.plates)))
end

-- Bare: no plate left. The court quakes (the game's Earthquake, his), and the sand-devils begin.
local function bare(c: any, now: number)
	local k = c.k
	if k.bare then
		return
	end
	local n = numbers.ruinWarden
	k.bare = true
	if not k.enraged then
		k.phase = 2
	end
	nox.unitSay(c.id, c.def.lines.bare, 90)
	local x, y = nox.position(c.id)
	nox.effect(MSG_EXPLOSION, x, y)
	for i = 0, 5 do
		local a = i * math.pi / 3
		nox.effect(MSG_SMOKE_BLAST, x + 100 * math.cos(a), y + 100 * math.sin(a))
	end
	if nox.cast("SPELL_EARTHQUAKE", c.id, nil, x, y, n.sand.quake) then
		k.quakes += 1
	end
	k.sandAt = now + 60
	redraw(c)
end

-- A pound that struck nobody: a plate shaken off him (while he has one), and he reels, open.
local function reel(c: any, now: number)
	local k = c.k
	local n = numbers.ruinWarden.plates
	local x, y = nox.position(c.id)
	local frames = n.staggerBare
	if k.plates > 0 then
		k.plates -= 1
		k.cracked += 1
		frames = n.stagger
		nox.effect(MSG_EXPLOSION, x, y)
		nox.effect(MSG_SMOKE_BLAST, x, y)
		nox.unitSay(c.id, c.def.lines.plate, 60)
		redraw(c)
	else
		nox.effect(MSG_SMOKE_BLAST, x, y)
	end
	k.reels += 1
	nox.enchant(c.id, "ENCHANT_HELD", frames)
	k.exposedUntil = now + n.exposedFor
	setTaken(c, now)
	if k.plates == 0 then
		bare(c, now)
	end
end

-- ---- The pound

-- A ring of sparks where a player within range stands, and a moment later the ground heaves
-- there; whether one began.
local function pound(c: any): boolean
	local k = c.k
	local n = numbers.ruinWarden.pound
	local x, y = nox.position(c.id)
	local around = playersNear(x, y, n.range)
	if #around == 0 then
		return false
	end
	local u = around[nox.random(#around)]
	local tx, ty = nox.position(u)
	nox.direction(c.id, nox.directionOf(tx - x, ty - y))
	nox.unitSay(c.id, c.def.lines.pound, 45)
	local function mark()
		nox.effect(MSG_YELLOW_SPARKS, tx, ty)
		for i = 0, 5 do
			local a = i * math.pi / 3
			nox.effect(MSG_YELLOW_SPARKS, tx + n.reach * 0.75 * math.cos(a), ty + n.reach * 0.75 * math.sin(a))
		end
	end
	mark()
	nox.after(n.warn // 2, function()
		if on(c) then
			mark()
		end
	end)
	nox.after(n.warn, function()
		if not on(c) then
			return
		end
		k.pounds += 1
		nox.effect(MSG_EXPLOSION, tx, ty)
		local caught = playersNear(tx, ty, n.reach)
		for _, p in caught do
			nox.applyForce(p, tx, ty, n.force)
		end
		-- (crushing, his own blow: combat.luau scales it as his hits; he and his wardens spared)
		nox.damageAt(tx, ty, n.reach, n.full, n.damage, CRUSH, c.id, nil, true)
		if #caught == 0 then
			k.misses += 1
			reel(c, nox.frame())
		else
			k.hits += #caught
		end
	end)
	return true
end

-- ---- The court's wardens

local function wardens(c: any)
	local k = c.k
	local n = numbers.ruinWarden.wardens
	local standing = nox.keepLiving(k.adds)
	local want = math.min(n.count + (if k.enraged then n.enraged else 0), n.most - standing - k.rising)
	if want <= 0 then
		return
	end
	local list: { string } = if k.bare and c.def.laterAdds then c.def.laterAdds else c.def.adds
	nox.unitSay(c.id, c.def.lines.wardens, 75)
	for _ = 1, want do
		local p = k.ring[k.nextPiece]
		k.nextPiece = k.nextPiece % #k.ring + 1
		nox.effect(MSG_CYAN_SPARKS, p.x, p.y)
		nox.effect(MSG_LESSER_EXPLOSION, p.x, p.y)
		k.rising += 1
		local t = list[nox.random(#list)]
		nox.after(n.warn, function()
			k.rising -= 1
			if not on(c) then
				return
			end
			local id = monsters.spawn(t, p.x, p.y, n.tier, c.info.level)
			if id then
				table.insert(k.adds, id)
				k.called += 1
				local ax, ay = nox.position(id)
				nox.effect(MSG_SMOKE_BLAST, ax, ay)
			end
		end)
	end
end

-- ---- The sand-devil (bare)

-- A sand-devil loosed at the arena's middle.
local function loose(c: any, now: number)
	local k = c.k
	local n = numbers.ruinWarden.sand
	nox.unitSay(c.id, c.def.lines.sand, 60)
	k.devil = { x = k.middle.x, y = k.middle.y, untilAt = now + n.lasts }
	k.devils += 1
	nox.effect(MSG_EXPLOSION, k.middle.x, k.middle.y)
end

-- The sand-devil on the beat: on toward the nearest player, its dust, whoever it catches slowed
-- and scoured.
local function whirl(c: any, now: number)
	local k = c.k
	local d = k.devil
	if not d then
		return
	end
	if now >= d.untilAt then
		k.devil = nil
		return
	end
	local n = numbers.ruinWarden.sand
	local best, bestD = nil, math.huge
	for _, u in playersNear(d.x, d.y, 800) do
		local ux, uy = nox.position(u)
		local dd = (ux - d.x) * (ux - d.x) + (uy - d.y) * (uy - d.y)
		if dd < bestD then
			best, bestD = u, dd
		end
	end
	if best then
		local ux, uy = nox.position(best)
		local len = math.sqrt(bestD)
		local step = math.min(len, n.speed * numbers.ruinWarden.beat)
		if len > 0 then
			local nx, ny = d.x + (ux - d.x) / len * step, d.y + (uy - d.y) / len * step
			-- (a wall stops it where it is)
			if nox.traceRay(d.x, d.y, nx, ny, 1) then
				d.x, d.y = nx, ny
			end
		end
	end
	-- (its dust: a turning ring of smoke round its middle)
	k.turn = (k.turn + 1) % 6
	nox.effect(MSG_SMOKE_BLAST, d.x, d.y)
	local a = k.turn * math.pi / 3
	nox.effect(MSG_SMOKE_BLAST, d.x + 30 * math.cos(a), d.y + 30 * math.sin(a))
	nox.effect(MSG_SMOKE_BLAST, d.x - 30 * math.cos(a), d.y - 30 * math.sin(a))
	local caught = playersNear(d.x, d.y, n.reach)
	if #caught > 0 then
		for _, u in caught do
			nox.enchant(u, "ENCHANT_SLOWED", n.slow)
		end
		nox.damageAt(d.x, d.y, n.reach, n.full, n.damage, CRUSH, c.id, nil, true)
		k.scoured += #caught
	end
end

-- ---- The court's masonry (enraged)

local function masonry(c: any)
	local k = c.k
	local n = numbers.ruinWarden.masonry
	local x, y = nox.position(c.id)
	local around = playersNear(x, y, 700)
	local count = math.min(#around, n.count)
	if count == 0 then
		return
	end
	nox.unitSay(c.id, c.def.lines.masonry, 60)
	for _ = 1, count do
		local u = table.remove(around, nox.random(#around)) :: number
		local px, py = nox.position(u)
		local a = nox.random() * 2 * math.pi
		local r = nox.random() * n.spread
		local sx, sy = nox.clearPointNear(px + r * math.cos(a), py + r * math.sin(a), 20)
		nox.effect(MSG_YELLOW_SPARKS, sx, sy)
		nox.effect(MSG_SMOKE_BLAST, sx, sy)
		nox.after(n.warn, function()
			if not on(c) then
				return
			end
			nox.effect(MSG_EXPLOSION, sx, sy)
			nox.damageAt(sx, sy, n.reach, n.full, n.damage, CRUSH, c.id, nil, true)
			k.stones += 1
		end)
	end
end

-- ---- The fight

-- His fight begun (`c`: boss.luau's, his id, def and area info; `spots`: the lair's). His plates
-- set on his entry at once (asleep too).
function warden.start(c: any, spots: any)
	local n = numbers.ruinWarden
	c.k = {
		awake = false,
		phase = 0,
		bare = false,
		enraged = false,
		plates = n.plates.count,
		cracked = 0,
		reels = 0,
		exposedUntil = -1,
		pounds = 0,
		misses = 0,
		hits = 0,
		adds = {},
		rising = 0,
		called = 0,
		quakes = 0,
		devils = 0,
		devil = nil,
		turn = 0,
		scoured = 0,
		stones = 0,
		ring = ringOf(spots),
		nextPiece = 1,
		middle = { x = spots.boss.x, y = spots.boss.y },
		poundAt = 0,
		wardensAt = 0,
		sandAt = 0,
		masonryAt = 0,
	}
	c.k.nextPiece = nox.random(#c.k.ring)
	setTaken(c, nox.frame())
end

-- His fight on the beat.
function warden.think(c: any)
	local k = c.k
	if not k or not on(c) then
		return
	end
	local n = numbers.ruinWarden
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
		k.poundAt = now + n.pound.every // 2
		k.wardensAt = now + n.wardens.every // 3
	end
	local share = o.health / math.max(1, o.maxHealth)
	-- (every plate left falls below its share: a party that can't make him miss still gets on)
	if k.plates > 0 and share <= n.plates.bareAt then
		k.cracked += k.plates
		k.plates = 0
		bare(c, now)
	end
	if not k.enraged and share <= n.enrage.at then
		k.enraged = true
		k.phase = 3
		nox.enchant(c.id, c.def.enrage, 0)
		nox.unitSay(c.id, c.def.lines.enrage, 90)
		if not k.bare then
			k.cracked += k.plates
			k.plates = 0
			bare(c, now)
		end
		k.masonryAt = now + n.masonry.every // 2
		k.poundAt = math.min(k.poundAt, now + n.pound.enragedEvery)
		k.sandAt = math.min(k.sandAt, now + n.sand.enragedEvery)
	end
	setTaken(c, now)
	if now >= k.poundAt then
		k.poundAt = now + (if pound(c) then (if k.enraged then n.pound.enragedEvery else n.pound.every) else 30)
	end
	if now >= k.wardensAt then
		k.wardensAt = now + n.wardens.every
		wardens(c)
	end
	if k.bare and not k.devil and now >= k.sandAt then
		k.sandAt = now + (if k.enraged then n.sand.enragedEvery else n.sand.every)
		loose(c, now)
	end
	whirl(c, now)
	if k.enraged and now >= k.masonryAt then
		k.masonryAt = now + n.masonry.every
		masonry(c)
	end
end

-- His fight over (his death, or his map gone): nothing more of it runs.
function warden.stop(c: any)
	c.over = true
	if c.k then
		c.k.devil = nil
	end
end

-- What "dd fight" says: "phase P awake|asleep, plates N, plated|open|bare, pounds P (misses M,
-- hits H), reels R, adds standing/called, quakes Q, devils D (scoured S), stones N, ring R".
function warden.line(c: any): string
	local k = c.k
	if not k then
		return "no fight"
	end
	local standing: number = nox.keepLiving(k.adds)
	local awake: string = if k.awake then "awake" else "asleep"
	local state: string = if nox.frame() < k.exposedUntil then "open" elseif k.plates > 0 then "plated" else "bare"
	return "phase " .. tostring(k.phase) .. " " .. awake .. ", plates " .. tostring(k.plates) .. ", " .. state .. ", pounds " .. tostring(k.pounds)
		.. " (misses " .. tostring(k.misses) .. ", hits " .. tostring(k.hits) .. "), reels " .. tostring(k.reels) .. ", adds " .. tostring(standing)
		.. "/" .. tostring(k.called) .. ", quakes " .. tostring(k.quakes) .. ", devils " .. tostring(k.devils) .. " (scoured " .. tostring(k.scoured)
		.. "), stones " .. tostring(k.stones) .. ", ring " .. tostring(#k.ring)
end

-- The fight's next blows brought forward, for "dd fight now": each comes on the next beat.
function warden.hurry(c: any)
	local k = c.k
	if k then
		k.poundAt, k.wardensAt, k.sandAt, k.masonryAt = 0, 0, 0, 0
	end
end

return warden
