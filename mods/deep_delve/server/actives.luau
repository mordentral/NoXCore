--!strict
-- The warrior's active abilities (by decision 2026-11-02): Leap Slam and Whirlwind, mod abilities
-- (nox.defineAbility), a button each on a NoX Core client's strip over the quickbar (client level
-- 8, the mod's minClientLevel). Their numbers are data/warrior.luau's `actives`, their tree nodes
-- data/tree.luau's (skill nodes: a warrior's alone, any other class gets the stats' worth).
--
--   Granted: each time a hero's bonus is worked out again (hero.onBonus: its set-up at a login
--     and at each map, an equip, a node taken or given back, a respec) each active its skills
--     hold is granted and any other taken back. The engine keeps a player's grants by peer through
--     map changes, never in the record: the tree is the record, and each set-up grants again.
--   Leap Slam: a press (onModAbility, at the cursor's point) leaps the warrior there (nox.leap;
--     at most `range` away, along the line); onLand: the slam about where it came down (a wall
--     may cut the leap short: it slams there).
--   Whirlwind: a press spins the warrior (nox.setBusy: no blows, spells or abilities; slowed; its
--     body drawn turning, nox.setSpin), a timer turning every `every` frames and striking the
--     monsters about it; its release or its hold's end stops it, and its cooldown runs from then
--     (nox.setAbilityCooldown).
--
-- Their hits are the warrior's skill hits (warrior.strike: quiet, the mod's monsters alone, so
-- never a party member or its creatures); Execute and Hunter's Mark count on each, and a landing
-- or a turn that strikes anything fills the rage (warrior.fillRage) without spending it.

local data = require("data/warrior")
local tipData = require("data/tips")
local hero = require("hero")
local items = require("items")
local monsters = require("monsters")
local tips = require("tips")
local warrior = require("warrior")

local actives = {}

-- The actives in the order their buttons are granted.
local ORDER = { "leapSlam", "whirlwind" }

for _, id in ORDER do
	local a: any = (data.actives :: any)[id]
	nox.defineAbility(id, { title = a.title, icon = a.icon, cooldown = a.cooldown, target = a.target, hold = a.hold })
end

local LEAP: any = data.actives.leapSlam
local WHIRL: any = data.actives.whirlwind

-- A hero's own state for its actives.
local function state(h: any): any
	local s = h.activeState
	if not s then
		s = { counts = {} }
		h.activeState = s
	end
	return s
end

local function count(s: any, what: string, n: number?)
	s.counts[what] = (s.counts[what] or 0) + (n or 1)
end

local function rankOf(h: any, id: string): number?
	return h.bonus and h.bonus.skills and h.bonus.skills[id]
end

-- The warrior's blow now: its worn melee weapon's at its strength (as the tooltip's "Damage"),
-- else bare-handed (data's `bare`); before its damage bonus.
function actives.blow(h: any): number
	local strength = nox.strength(h.unit)
	local weapon = h.bonus and h.bonus.weapon
	if weapon and not h.bonus.ammo then
		local o = nox.object(weapon)
		local def = if o then (tipData.items :: any)[o.type] else nil
		local st = if o then nox.itemStats(o.type) else nil
		local rec = items.recordOf(weapon)
		if rec and st and st.damageMin and st.damageMin > 0 and not (def and def.kind == "spell staff") then
			return tips.damage(rec, st, strength, false)
		end
	end
	return math.max(data.bare.least, math.floor(strength * data.bare.perStrength + 0.5))
end

-- Strikes every monster within `reach` of (x, y) for `pct` of the blow; how many it struck and
-- what they took.
local function strikeAll(h: any, x: number, y: number, reach: number, pct: number, kind: number): (number, number, number)
	local base = actives.blow(h) * hero.dealt(h, nil) * pct / 100
	local hit, dealt = 0, 0
	for _, id in warrior.monstersNear(x, y, reach, nil, 32) do
		local ox, oy = nox.position(id)
		local before = nox.object(id)
		local was = if before then before.health else 0
		warrior.strike(h, id, base * warrior.factor(h, id), kind)
		local after = nox.object(id)
		dealt += math.max(0, was - (if after then after.health else 0))
		nox.effect(data.fx.slamHit, ox, oy)
		hit += 1
	end
	return hit, dealt, base
end

-- ---- Leap Slam

local function leap(h: any, s: any, x: number, y: number): boolean
	local rank = rankOf(h, "leapSlam")
	if not rank or s.spin then
		return false
	end
	local unit = h.unit
	local ux, uy = nox.position(unit)
	local dx, dy = x - ux, y - uy
	local d = math.sqrt(dx * dx + dy * dy)
	if d > LEAP.range then
		x, y = ux + dx * LEAP.range / d, uy + dy * LEAP.range / d
	end
	if d >= 1 then
		nox.direction(unit, nox.directionOf(dx, dy))
	end
	local lx, ly = nox.leap(unit, x, y, { time = LEAP.time, height = LEAP.height })
	if not lx or not ly then
		count(s, "leapRefused")
		return false
	end
	s.leap = { rank = rank, at = nox.frame(), x = lx, y = ly }
	nox.sound(unit, data.sounds.leap)
	count(s, "leap")
	return true
end

-- Down (onLand): the slam about the landing point.
function actives.landed(unit: number, x: number, y: number, cut: boolean)
	local h = hero.byUnit(unit)
	local s = if h then h.activeState else nil
	local l = if s then s.leap else nil
	if not h or not s or not l then
		return
	end
	s.leap = nil
	local n = data.numbers("leapSlam", l.rank)
	nox.effect(data.fx.slam, x, y)
	nox.sound(unit, data.sounds.slam)
	local hit, dealt, base = strikeAll(h, x, y, n.radius, n.pct, LEAP.kind)
	-- (each one struck and still standing: thrown back, then held a moment once the throw has
	-- carried it (the game's hold stops a thing's motion); one held already is kept held, unthrown;
	-- a boss never thrown, held less)
	for _, id in warrior.monstersNear(x, y, n.radius, nil, 32) do
		local m = monsters.info(id)
		local boss = m ~= nil and m.tier == "boss"
		local frames = if boss then n.bossHeld else n.held
		local after = 1
		if not boss and not nox.enchanted(id, "ENCHANT_HELD") then
			nox.applyForce(id, x, y, n.force)
			after = LEAP.holdAfter
		end
		nox.after(after, function()
			if nox.living(id) and nox.enchantTime(id, "ENCHANT_HELD") < frames then
				nox.enchant(id, "ENCHANT_HELD", frames, 1)
			end
		end)
	end
	count(s, "slam")
	count(s, "slamHit", hit)
	s.lastSlam = { x = x, y = y, hit = hit, dealt = dealt, base = base, cut = cut }
	if hit > 0 then
		warrior.fillRage(h, LEAP.rage)
	end
end

-- ---- Whirlwind

-- The warrior's body turning while it spins (nox.setSpin: drawn by NoX Core clients of level 8,
-- nothing changes on the server): `frames` at its start, 0 at its stop.
local function spinBody(unit: number, frames: number)
	nox.setSpin(unit, if frames > 0 then WHIRL.turns else 0, frames)
end

local function stopSpin(h: any, s: any, why: string)
	local spin = s.spin
	if not spin then
		return
	end
	s.spin = nil
	nox.cancelTimer(spin.timer)
	local unit = h.unit
	if nox.present(unit) then
		nox.setBusy(unit, 0)
		-- (its own slow taken off; a longer one, a monster's frost, kept)
		local left = nox.enchantTime(unit, "ENCHANT_SLOWED")
		if left > 0 and left <= spin.slowEnds - nox.frame() + 1 then
			nox.enchant(unit, "ENCHANT_SLOWED", -1)
		end
		nox.setAbilityCooldown(unit, "whirlwind", WHIRL.cooldown)
		spinBody(unit, 0)
	end
	count(s, "spinEnd")
	s.lastSpin = { turns = spin.turns, hits = spin.hits, dealt = spin.dealt, frames = nox.frame() - spin.from, why = why }
end

local function turn(h: any, s: any, spin: any)
	local unit = h.unit
	local n = data.numbers("whirlwind", spin.rank)
	local x, y = nox.position(unit)
	spin.turns += 1
	if spin.turns % 2 == 1 then
		nox.effect(data.fx.whirl, x, y)
	end
	nox.sound(unit, data.sounds.turn)
	local hit, dealt = strikeAll(h, x, y, n.reach, n.pct, WHIRL.kind)
	spin.hits += hit
	spin.dealt += dealt
	count(s, "turn")
	if hit > 0 then
		warrior.fillRage(h, WHIRL.rage)
	end
end

local function spin(h: any, s: any): boolean
	local rank = rankOf(h, "whirlwind")
	if not rank or s.spin or s.leap then
		return false
	end
	local unit = h.unit
	local peer = h.peer
	local frames = WHIRL.hold + 2
	if not nox.setBusy(unit, frames) then
		return false
	end
	local now = nox.frame()
	if nox.enchantTime(unit, "ENCHANT_SLOWED") < frames and not nox.enchanted(unit, "ENCHANT_SLOWED") then
		nox.enchant(unit, "ENCHANT_SLOWED", frames)
	end
	local sp: any = { rank = rank, turns = 0, hits = 0, dealt = 0, from = now, slowEnds = now + frames }
	s.spin = sp
	count(s, "spin")
	spinBody(unit, frames)
	turn(h, s, sp)
	sp.timer = nox.every(WHIRL.every, function()
		if s.spin ~= sp then
			nox.cancelTimer(sp.timer)
		elseif hero.byPeer(peer) ~= h or not nox.living(unit) then
			stopSpin(h, s, "down")
		elseif nox.frame() - sp.from >= frames then
			stopSpin(h, s, "time")
		else
			turn(h, s, sp)
		end
	end)
	return true
end

-- ---- The hooks

-- A granted ability used (onModAbility): false refuses a press (no cooldown starts).
function actives.used(unit: number, id: string, x: number?, y: number?, phase: string): boolean?
	local h = hero.byUnit(unit)
	if not h or not h.ready then
		return false
	end
	local s = state(h)
	if id == "leapSlam" then
		if phase ~= "press" or not x or not y then
			return nil
		end
		return leap(h, s, x, y)
	elseif id == "whirlwind" then
		if phase == "press" then
			return spin(h, s)
		end
		stopSpin(h, s, phase)
		return nil
	end
	return false
end

-- What the hero has changed (hero.onBonus): each active its skills hold granted, any other taken
-- back (a spin under way stopped).
function actives.refresh(h: any)
	local unit = h.unit
	if not nox.present(unit) then
		return
	end
	local sk = h.bonus and h.bonus.skills or {}
	for _, id in ORDER do
		if sk[id] then
			-- (nil: nothing granted, the strip full or the player's client one that can't show it;
			-- tried again at the next change)
			nox.grantAbility(unit, id)
		else
			if id == "whirlwind" and h.activeState then
				stopSpin(h, h.activeState, "taken back")
			end
			nox.revokeAbility(unit, id)
		end
	end
end
hero.onBonus(actives.refresh)

-- A new map: no leap under way (the engine let them go), a spin stopped.
function actives.reset()
	for _, h in hero.all() do
		local s = h.activeState
		if s then
			s.leap = nil
			stopSpin(h, s, "map")
		end
	end
end

-- "dd actives": the first hero's actives (rank, cooldown), the blow they're shares of, the counts,
-- the last slam and spin; "clear": the counts emptied; "ready": both cooldowns ended.
function actives.console(args: { string }, h: any): { string }
	if not h then
		return { "no player" }
	end
	local s = state(h)
	if args[2] == "clear" then
		table.clear(s.counts)
		s.lastSlam = nil
		s.lastSpin = nil
		return { "cleared" }
	elseif args[2] == "ready" then
		for _, id in ORDER do
			nox.setAbilityCooldown(h.unit, id, 0)
		end
		return { "ready" }
	end
	local out = {}
	for _, id in ORDER do
		table.insert(out, string.format("active %s rank %d cooldown %d", id, rankOf(h, id) or 0, nox.abilityCooldown(h.unit, id)))
	end
	table.insert(out, "blow " .. tostring(actives.blow(h)) .. " dealt " .. string.format("%.2f", hero.dealt(h, nil)))
	local what = {}
	for k in s.counts do
		table.insert(what, k)
	end
	table.sort(what)
	for _, k in what do
		table.insert(out, "count " .. k .. " " .. tostring(s.counts[k]))
	end
	local l: any = s.lastSlam
	if l then
		table.insert(out, string.format("slam %d %d hit %d dealt %d base %d cut %s", math.floor(l.x :: number), math.floor(l.y :: number), l.hit :: number, l.dealt :: number, math.floor((l.base :: number) + 0.5),
			tostring(l.cut)))
	end
	table.insert(out, "spinning " .. (if s.spin then "yes" else "no") .. " busy " .. tostring(nox.busy(h.unit)))
	local p: any = s.lastSpin
	if p then
		table.insert(out, string.format("spun %d turns %d hits dealt %d frames %d %s", p.turns :: number, p.hits :: number, p.dealt :: number, p.frames :: number, tostring(p.why)))
	end
	return out
end

return actives
