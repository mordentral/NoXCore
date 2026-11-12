--!strict
-- The warrior's passive skills at work (data/warrior.luau's numbers; the nodes are
-- data/tree.luau's). A hero's bonus carries `skills` (key -> rank: tree.totals, a warrior's
-- alone), worked out again at each set-up, equip and tree change; each skill is looked up there
-- by its key, never by a walk.
--
-- By the hooks they need:
--   a hero's hit on one of the mod's monsters (combat.luau, warrior.dealt): Hunter's Mark raises
--     any party member's hit on a monster a marking warrior sees; on the warrior's own blow (the
--     cause "blow", its unit the source) Momentum, Execute and Ambush raise it; once combat has
--     the blow's last amount (warrior.landed) what follows it is done a frame on (an asked hook
--     changes nothing): Cleave's splash, Rend's and the axe's bleed, the hammer's shockwave, the
--     staff's push, the rage's fill or its spin; a thrown chakram's hit strikes the next monster;
--     Envenom's poison (by decision 2026-11-12) on any hit of the warrior's weapon (its blow, a
--     thrown chakram; a worn bow's arrow too, should one be worn);
--   a monster's hit on the warrior (combat.luau, warrior.taken): a sword's parry turns a blow,
--     Shield Wall staggers the monster that struck (the game's hold), the rage fills;
--   a hit taken, after it (main.luau's onDamaged, warrior.hurt): Second Wind below its share;
--   time (a beat per hero with a timed skill, data's `beat`): the bleeds' ticks, Second Wind's
--     heal, the rage's drain, Ambush's watch on Tread Lightly's sneak, Hunter's Mark's rings.
--
-- Damage a skill deals itself is `quiet` (combat.luau deals it as it is: a share of a blow that
-- had every bonus in it already); no skill or worn power rolls on it again. The party rule holds:
-- only the mod's monsters are struck, and combat.luau cancels any hit within the party first.
--
-- The Rage bar: the hero's rage to its UI (hud.luau, "rage" { v, most }) only when its whole
-- number changes; v = -1 closes the bar (no Rage skill).

local data = require("data/warrior")
local tipData = require("data/tips")
local affixes = require("affixes")
local hero = require("hero")
local hud = require("hud")
local monsters = require("monsters")
local secrets = require("secrets")
local decals = require("decals")

local warrior = {}

local FPS = 30
local MONSTERS: any = { monsters = true, alive = true }

-- Damage a skill deals itself (combat.luau deals it as it is).
local quiet = false

function warrior.quiet(): boolean
	return quiet
end

-- The weapon families by an item type's kind (data/tips.luau's), and by a thrown weapon's missile.
local familyOfKind: { [string]: string } = {}
local familyOfThrown: { [string]: string } = {}
for key, m in data.mastery :: any do
	for _, k in m.kinds do
		familyOfKind[k] = key
	end
	for _, t in m.thrown or {} do
		familyOfThrown[t] = key
	end
end

-- An item type's family ("sword", "hammer", ...), nil for none.
function warrior.familyOf(thingType: string): string?
	local t = (tipData.items :: any)[thingType]
	return t and familyOfKind[t.kind]
end

-- The heroes with Hunter's Mark (by unit), kept at each refresh.
local markers: { [number]: any } = {}

-- A hero's own state for its skills.
local function state(h: any): any
	local s = h.warriorState
	if not s then
		s = { counts = {}, bleeds = {}, bleedCount = 0, rage = 0, rageSent = -2, rings = {} }
		h.warriorState = s
	end
	return s
end

local function count(h: any, what: string)
	local c = state(h).counts
	c[what] = (c[what] or 0) + 1
end

local function skills(h: any): { [string]: number }
	return h.bonus and h.bonus.skills or {}
end

-- A skill's numbers for the hero (its rank's), or nil when it hasn't the skill.
local function skill(h: any, key: string): any
	local rank = skills(h)[key]
	return if rank then data.numbers(key, rank) else nil
end

-- Damage from the hero to one of the mod's monsters, as a skill deals it (quiet; its share of a
-- blow the monster takes: the ruin-warden's plates); what it took.
local function strike(h: any, target: number, amount: number, kind: number): number
	local m = monsters.info(target)
	if not m or amount <= 0 or not nox.living(target) or not nox.present(h.unit) then
		return 0
	end
	local share = if type(m.taken) == "number" then m.taken else 1
	local dealt = math.max(1, math.floor(amount * share + 0.5))
	quiet = true
	local ok, took = pcall(nox.damage, target, h.unit, dealt, kind)
	quiet = false
	if not ok then
		nox.log("deep_delve: a skill's hit not dealt: " .. tostring(took))
		return 0
	end
	return took
end

-- The mod's monsters within `reach` of (x, y) but `except`, nearest first, at most `most`.
local function monstersNear(x: number, y: number, reach: number, except: number?, most: number): { number }
	local found: { any } = {}
	for _, id in nox.unitsNear(x, y, reach, MONSTERS) do
		if id ~= except and monsters.info(id) and nox.living(id) then
			local ox, oy = nox.position(id)
			table.insert(found, { id = id, d = (ox - x) * (ox - x) + (oy - y) * (oy - y) })
		end
	end
	table.sort(found, function(a: any, b: any): boolean
		return a.d < b.d
	end)
	local out = {}
	for i = 1, math.min(most, #found) do
		out[i] = found[i].id
	end
	return out
end

-- ---- The Rage bar

local function sendRage(h: any)
	local s = state(h)
	local v = if skills(h).rage then math.floor(s.rage) else -1
	if v ~= s.rageSent then
		s.rageSent = v
		hud.send(h.peer, "rage", { kind = "rage", v = v, most = data.skills.rage.most })
	end
end

local function addRage(h: any, n: number)
	local s = state(h)
	local r = data.skills.rage
	local was = s.rage
	s.rage = math.min(r.most, s.rage + n)
	s.rageAt = nox.frame()
	if s.rage >= r.most and was < r.most then
		count(h, "rageFull")
		local x, y = nox.position(h.unit)
		nox.effect(data.fx.rageFull, x, y)
	end
	sendRage(h)
end

-- ---- Bleeds (Rend's and the axe's)

local function bleed(h: any, target: number, total: number, seconds: number)
	local ticks = math.max(1, math.floor(seconds * FPS / data.bleedEvery))
	if total <= 0 then
		return
	end
	local s = state(h)
	local b = s.bleeds[target]
	-- (the stronger of the two runs: the one dealing more a tick)
	if b and b.pool / b.ticks >= total / ticks then
		return
	end
	if not b then
		if s.bleedCount >= data.bleedMost then
			-- (the oldest let go)
			local oldest, at = nil, math.huge
			for id, e in s.bleeds do
				if e.since < at then
					oldest, at = id, e.since
				end
			end
			if oldest then
				s.bleeds[oldest] = nil
				s.bleedCount -= 1
			end
		end
		s.bleedCount += 1
	end
	local now = nox.frame()
	s.bleeds[target] = { pool = total, ticks = ticks, carry = 0, next = now + data.bleedEvery, since = now }
	count(h, "bleed")
end

local function tickBleeds(h: any, s: any, now: number)
	for id, b in s.bleeds do
		if not nox.living(id) then
			s.bleeds[id] = nil
			s.bleedCount -= 1
		elseif now >= b.next then
			local share = b.pool / b.ticks + b.carry
			local whole = math.floor(share)
			b.carry = share - whole
			b.pool -= b.pool / b.ticks
			b.ticks -= 1
			b.next = now + data.bleedEvery
			if whole >= 1 then
				strike(h, id, whole, 0)
				count(h, "bleedTick")
			end
			if b.ticks <= 0 and s.bleeds[id] == b then
				s.bleeds[id] = nil
				s.bleedCount -= 1
			end
		end
	end
end

-- ---- A hero's hit on one of the mod's monsters (combat.luau)

-- The blow being dealt now (warrior.dealt's), finished by warrior.landed with its last amount.
local pending: any = nil

-- Hunter's Mark: whether a marking warrior with its sight on sees the monster (none: 1).
local function marked(target: number): number
	if next(markers) == nil then
		return 1
	end
	local hm = data.skills.huntersMark
	local tx, ty = nox.position(target)
	for unit, h in markers do
		if nox.living(unit) and nox.enchanted(unit, "ENCHANT_INFRAVISION") then
			local x, y = nox.position(unit)
			local dx, dy = tx - x, ty - y
			if dx * dx + dy * dy <= hm.sight * hm.sight and nox.traceRay(x, y, tx, ty, 1) then
				count(h, "marked")
				return 1 + hm.pct / 100
			end
		end
	end
	return 1
end

-- The hit's extra factor (1: none): `h` the hero behind it, `source` and `weapon` onDamage's,
-- `factor` what the hit is scaled by already, `cause` onDamage's.
function warrior.dealt(h: any, target: number, m: any, source: number?, weapon: number?, amount: number, factor: number, cause: string, kind: number): number
	pending = nil
	if quiet then
		return 1
	end
	local mult = marked(target)
	local sk = skills(h)
	if next(sk) == nil then
		return mult
	end
	local unit = h.unit
	local now = nox.frame()
	local s = state(h)
	local env = skill(h, "envenom")
	if cause == "missile" and weapon then
		-- (a thrown chakram's hit: its family's by the missile in flight; Envenom on it, and on the
		-- worn bow's arrow)
		local o = nox.object(weapon)
		local thrown = if o and o.owner == unit then familyOfThrown[o.type] else nil
		local fam = if thrown and sk[thrown] then thrown else nil
		local venom = if env and (thrown or affixes.byWeapon(h.bonus, unit, weapon, cause)) then env.most else nil
		if fam or venom then
			pending = { h = h, target = target, missile = true, thrown = fam, kind = kind, venom = venom }
		end
		return mult
	end
	if cause ~= "blow" or source ~= unit then
		return mult
	end
	-- (the warrior's own blow)
	local mo = skill(h, "momentum")
	if mo then
		local row = s.momentum
		if row and row.target == target and now - row.at <= mo.seconds * FPS then
			row.n += 1
		else
			row = { target = target, n = 0 }
			s.momentum = row
		end
		row.at = now
		if row.n > 0 then
			mult *= 1 + math.min(row.n * mo.step, mo.most) / 100
			count(h, "momentum")
		end
	end
	local ex = skill(h, "execute")
	if ex then
		local o = nox.object(target)
		if o and o.maxHealth > 0 and o.health * 100 < o.maxHealth * ex.below then
			mult *= ex.times
			count(h, "execute")
		end
	end
	local am = skill(h, "ambush")
	local sneaking = am ~= nil and nox.enchanted(unit, "ENCHANT_SNEAK")
	if am and (s.ambushArmed or sneaking) then
		s.ambushArmed = false
		if sneaking or now - (s.sneakSeen or -1e9) <= am.window then
			mult *= am.times
			count(h, "ambush")
		end
	end
	-- (the worn weapon's own blow: its family)
	local fam: string? = nil
	if weapon and h.bonus.weapon == weapon then
		local o = nox.object(weapon)
		fam = if o then warrior.familyOf(o.type) else nil
	end
	-- (its facing as the blow lands: Cleave's arc; Envenom's most on a blow of its worn weapon)
	local venom = if env and affixes.byWeapon(h.bonus, unit, weapon, cause) then env.most else nil
	pending = { h = h, target = target, family = if fam and sk[fam] then fam else nil, kind = kind, face = nox.direction(unit), venom = venom }
	return mult
end

-- Envenom: the monster's poison a level more, to at most `most` (the game's Poison spell from the
-- warrior at the level it is to reach: a monster immune to poison, or resisting it, takes none, as
-- the game's Venom's blow), the warrior noted as its poisoner (combat.luau: its poison stat on the
-- bites).
local function envenom(h: any, target: number, most: number)
	local m = monsters.info(target)
	if not m or not nox.living(target) then
		return
	end
	local level = nox.poison(target)
	if level >= most then
		m.poisoner = h.unit
		return
	end
	local x, y = nox.position(target)
	nox.cast("SPELL_POISON", h.unit, target, x, y, level + 1)
	if nox.poison(target) > level then
		m.poisoner = h.unit
		count(h, "envenom")
	end
end

-- What follows a blow (Cleave, the bleeds, the shockwave, the push, the rage), a frame on.
local function follow(p: any, final: number)
	local h = p.h
	local unit = h.unit
	if not nox.living(unit) then
		return
	end
	local target = p.target
	local s = state(h)
	local hx, hy = nox.position(unit)
	-- Envenom: the weapon's hit poisons.
	if p.venom then
		envenom(h, target, p.venom)
	end
	if p.missile then
		-- (a chakram's hit: the next monster near its target; nothing else follows a missile)
		local c = if p.thrown then skill(h, p.thrown) else nil
		if c and nox.present(target) then
			local tx, ty = nox.position(target)
			local other = monstersNear(tx, ty, c.reach, target, 1)[1]
			if other then
				local nx, ny = nox.position(other)
				nox.effect(data.fx.chakram, nx, ny)
				strike(h, other, final * c.pct / 100, p.kind)
				count(h, "chakram")
			end
		end
		return
	end
	-- Cleave: the monsters beside the target in front of the warrior.
	local cl = skill(h, "cleave")
	if cl then
		local face = p.face
		local hit = 0
		for _, id in monstersNear(hx, hy, cl.reach, target, 16) do
			if hit >= cl.most then
				break
			end
			local ox, oy = nox.position(id)
			local off = (nox.directionOf(ox - hx, oy - hy) - face) % 256
			if off > 128 then
				off = 256 - off
			end
			if off <= cl.arc then
				hit += 1
				nox.effect(data.fx.cleave, ox, oy)
				strike(h, id, final * cl.pct / 100, p.kind)
			end
		end
		if hit > 0 then
			count(h, "cleave")
		end
	end
	-- The bleeds: Rend's, the axe's (the stronger runs).
	if nox.living(target) then
		local rd = skill(h, "rend")
		if rd then
			bleed(h, target, final * rd.pct / 100, rd.seconds)
		end
		if p.family == "axe" then
			local ax = skill(h, "axe")
			bleed(h, target, final * ax.pct / 100, ax.seconds)
		end
	end
	-- The weapon's family.
	if p.family == "hammer" then
		local hm = skill(h, "hammer")
		s.hammerBlows = (s.hammerBlows or 0) + 1
		if s.hammerBlows >= hm.every and nox.present(target) then
			s.hammerBlows = 0
			local tx, ty = nox.position(target)
			nox.effect(data.fx.shockwave, tx, ty)
			count(h, "shockwave")
			for _, id in monstersNear(tx, ty, hm.reach, target, 16) do
				strike(h, id, final * hm.pct / 100, p.kind)
				if hm.held and nox.living(id) and monsters.info(id).tier ~= "boss" then
					nox.enchant(id, "ENCHANT_HELD", hm.held, 1)
				end
			end
		end
	elseif p.family == "staff" and nox.living(target) then
		local st = skill(h, "staff")
		local m = monsters.info(target)
		if m and m.tier ~= "boss" then
			nox.applyForce(target, hx, hy, st.force)
			if st.held then
				nox.enchant(target, "ENCHANT_HELD", st.held, 1)
			end
			count(h, "push")
		end
	end
	-- The rage: full, this blow spins; else it fills.
	local rg = skill(h, "rage")
	if rg then
		if s.rage >= rg.most then
			s.rage = 0
			s.rageAt = nox.frame()
			count(h, "spin")
			nox.effect(data.fx.spin, hx, hy)
			-- (the warrior turned once round as it spins: its facing in four steps)
			local face = nox.direction(unit)
			for i = 1, 4 do
				nox.after(i, function()
					if nox.living(unit) then
						nox.direction(unit, (face + 64 * i) % 256)
					end
				end)
			end
			for _, id in monstersNear(hx, hy, rg.reach, nil, 32) do
				strike(h, id, final * rg.times, p.kind)
			end
			sendRage(h)
		else
			addRage(h, rg.dealt)
		end
	end
end

-- The blow's last amount (combat.luau, once it has it): what follows it, a frame on.
function warrior.landed(final: number)
	local p = pending
	pending = nil
	if not p or final <= 0 then
		return
	end
	nox.after(1, function()
		follow(p, final)
	end)
end

-- ---- What the actives (actives.luau) share with the skills

-- An active's hit from the hero (quiet, as a skill's own; its share of a blow the monster takes).
warrior.strike = strike
-- A bleed of `total` over `seconds` on a monster from the hero (Rend's; the stronger runs): the
-- Harpoon's Reel and Rend (upgrades.luau).
warrior.bleed = bleed
-- The mod's monsters within `reach` of (x, y), nearest first, at most `most`.
warrior.monstersNear = monstersNear

-- The factor on an active's hit on a monster: Execute's below its share, Hunter's Mark's on a
-- monster a marking warrior sees (1: neither). Momentum and Ambush are the blow's alone.
function warrior.factor(h: any, target: number): number
	local mult = marked(target)
	local ex = skill(h, "execute")
	if ex then
		local o = nox.object(target)
		if o and o.maxHealth > 0 and o.health * 100 < o.maxHealth * ex.below then
			mult *= ex.times
			count(h, "execute")
		end
	end
	return mult
end

-- Rage from an active's hits: filled as by a blow (`n`), never spent by them.
function warrior.fillRage(h: any, n: number)
	if skill(h, "rage") and n > 0 then
		addRage(h, n)
	end
end

-- ---- A monster's hit on the warrior (combat.luau): its new amount, or nil (as it was)

function warrior.taken(h: any, who: number?, source: number?, weapon: number?, amount: number, cause: string): number?
	local sk = skills(h)
	if quiet or next(sk) == nil or amount <= 0 or not who or not monsters.info(who) then
		return nil
	end
	local unit = h.unit
	local blow = cause == "blow" and who == source
	if blow then
		-- A sword in hand: a parry turns the blow.
		local sw = skill(h, "sword")
		if sw and h.bonus.weapon then
			local o = nox.object(h.bonus.weapon)
			if o and warrior.familyOf(o.type) == "sword" and nox.random() * 100 < sw.chance then
				count(h, "parry")
				local x, y = nox.position(unit)
				nox.effect(data.fx.parry, x, y)
				return 0
			end
		end
		-- A shield worn: Shield Wall's stagger (Phalanx's chance with it).
		local wall = skill(h, "shieldWall")
		if wall and h.bonus.shield then
			local px = skill(h, "phalanx")
			local chance = wall.chance + (if px then px.chance else 0)
			if nox.random() * 100 < chance then
				count(h, "stagger")
				local m = monsters.info(who)
				local frames = if m.tier == "boss" then wall.bossFrames else wall.frames
				nox.after(1, function()
					if nox.living(who) then
						local ox, oy = nox.position(who)
						nox.effect(data.fx.stagger, ox, oy)
						if nox.enchantTime(who, "ENCHANT_HELD") < frames then
							nox.enchant(who, "ENCHANT_HELD", frames, 1)
						end
					end
				end)
			end
		end
	end
	local rg = skill(h, "rage")
	if rg then
		nox.after(1, function()
			if nox.living(unit) and skills(h).rage then
				addRage(h, rg.taken)
			end
		end)
	end
	return nil
end

-- ---- A hit taken, after it (main.luau's onDamaged): Second Wind

function warrior.hurt(h: any)
	local sw = skill(h, "secondWind")
	if not sw then
		return
	end
	local s = state(h)
	local now = nox.frame()
	if now < (s.windAt or 0) or s.wind then
		return
	end
	local o = nox.object(h.unit)
	if not o or not o.alive or o.health <= 0 or o.health * 100 >= o.maxHealth * sw.below then
		return
	end
	local ticks = math.max(1, math.floor(sw.seconds * FPS / data.bleedEvery))
	s.wind = { pool = o.maxHealth * sw.pct / 100, ticks = ticks, carry = 0, next = now }
	s.windAt = now + sw.every * FPS
	count(h, "secondWind")
	nox.effect(data.fx.secondWind, o.x, o.y)
	nox.tell(h.unit, "Second wind!")
end

local function tickWind(h: any, s: any, now: number)
	local w = s.wind
	if not w or now < w.next then
		return
	end
	if not nox.living(h.unit) then
		s.wind = nil
		return
	end
	local share = w.pool / w.ticks + w.carry
	local whole = math.floor(share)
	w.carry = share - whole
	w.pool -= w.pool / w.ticks
	w.ticks -= 1
	w.next = now + data.bleedEvery
	if whole >= 1 then
		nox.heal(h.unit, whole)
	end
	if w.ticks <= 0 then
		s.wind = nil
	end
end

-- ---- Hunter's Mark's rings

local function clearRings(s: any)
	for _, id in s.rings do
		decals.remove(id)
	end
	table.clear(s.rings)
end

local function rings(h: any, s: any)
	local hm = skill(h, "huntersMark")
	local on = hm ~= nil and nox.living(h.unit) and nox.enchanted(h.unit, "ENCHANT_INFRAVISION")
	if not on then
		if next(s.rings) ~= nil then
			clearRings(s)
		end
		return
	end
	local x, y = nox.position(h.unit)
	local reach = (hm :: any).reveal
	for _, found in secrets.hidden() :: { any } do
		local t: any = found
		local tx: number, ty: number = t.x, t.y
		local key = tostring(t.kind) .. " " .. tostring(math.floor(tx)) .. " " .. tostring(math.floor(ty))
		local dx, dy = tx - x, ty - y
		if not s.rings[key] and dx * dx + dy * dy <= reach * reach then
			local id = decals.put("mark_" .. t.kind, t.x, t.y, (data.reveal :: any)[t.kind])
			if id then
				s.rings[key] = id
				count(h, "revealed")
			end
		end
	end
end

-- ---- Time: the hero's beat

local function beat(h: any)
	local s = state(h)
	local now = nox.frame()
	if next(s.bleeds) ~= nil then
		tickBleeds(h, s, now)
	end
	tickWind(h, s, now)
	local sk = skills(h)
	if sk.ambush and nox.present(h.unit) and nox.enchanted(h.unit, "ENCHANT_SNEAK") then
		s.sneakSeen = now
		s.ambushArmed = true
	end
	local rg = data.skills.rage
	if s.rage > 0 and now - (s.rageAt or 0) >= rg.idle * FPS then
		s.rage = math.max(0, s.rage - rg.drain * data.beat / FPS)
		sendRage(h)
	end
	-- (Hunter's Mark's rings: looked at twice a second)
	if (sk.huntersMark or next(s.rings) ~= nil) and now % 15 < data.beat then
		rings(h, s)
	end
end

local TIMED = { rage = true, rend = true, axe = true, secondWind = true, ambush = true, huntersMark = true, reelAndRend = true }

local function timed(h: any, s: any): boolean
	for key in skills(h) do
		if TIMED[key] then
			return true
		end
	end
	return next(s.bleeds) ~= nil or s.wind ~= nil or s.rage > 0 or next(s.rings) ~= nil
end

-- What the hero has changed (hero.onBonus): its beat, its marks, its Rage bar.
function warrior.refresh(h: any)
	local s = state(h)
	local sk = skills(h)
	for unit, other in markers do
		if other == h and unit ~= h.unit then
			markers[unit] = nil -- (its unit before this one)
		end
	end
	if sk.huntersMark then
		markers[h.unit] = h
	else
		markers[h.unit] = nil
	end
	if not sk.rage and s.rage > 0 then
		s.rage = 0
	end
	-- (the bar: a hero that never had Rage is sent nothing)
	if sk.rage or s.rageSent ~= -2 then
		sendRage(h)
	end
	if timed(h, s) and not s.beat then
		local peer = h.peer
		local unit = h.unit
		local timer: number = 0
		timer = nox.every(data.beat, function()
			if hero.byPeer(peer) ~= h or s.beat ~= timer then
				nox.cancelTimer(timer)
				if s.beat == timer then
					s.beat = nil
				end
				markers[unit] = nil
				clearRings(s)
				return
			end
			beat(h)
			if not timed(h, s) then
				nox.cancelTimer(timer)
				s.beat = nil
			end
		end)
		s.beat = timer
	end
end
hero.onBonus(warrior.refresh)

-- A new map: the old map's monsters and rings went with it.
function warrior.reset()
	for _, h in hero.all() do
		local s = h.warriorState
		if s then
			table.clear(s.bleeds)
			s.bleedCount = 0
			s.momentum = nil
			table.clear(s.rings) -- (the host cleared the map's decals)
		end
	end
	for unit, h in markers do
		if hero.byUnit(unit) ~= h then
			markers[unit] = nil
		end
	end
end

-- What "dd warrior" says of a hero: each skill and its rank ("skill KEY RANK"), its counts
-- ("count WHAT N"), its state (rage, momentum, the bleeds, Second Wind's wait, the rings).
function warrior.lines(h: any): { string }
	local out = {}
	local sk = skills(h)
	local keys = {}
	for k in sk do
		table.insert(keys, k)
	end
	table.sort(keys)
	for _, k in keys do
		table.insert(out, "skill " .. k .. " " .. tostring(sk[k]))
	end
	local s = state(h)
	local what = {}
	for k in s.counts do
		table.insert(what, k)
	end
	table.sort(what)
	for _, k in what do
		table.insert(out, "count " .. k .. " " .. tostring(s.counts[k]))
	end
	local now = nox.frame()
	table.insert(out, string.format("rage %d sent %d", math.floor(s.rage), s.rageSent))
	if s.momentum then
		table.insert(out, string.format("momentum %d %d", s.momentum.target, s.momentum.n))
	end
	table.insert(out, "bleeds " .. tostring(s.bleedCount))
	table.insert(out, "wind " .. (if s.wind then "on" else "off") .. " wait " .. tostring(math.max(0, (s.windAt or 0) - now)))
	local n = 0
	for _ in s.rings do
		n += 1
	end
	table.insert(out, "rings " .. tostring(n))
	table.insert(out, "beat " .. (if s.beat then "on" else "off"))
	table.insert(out, "family " .. tostring(if h.bonus and h.bonus.weapon and nox.object(h.bonus.weapon)
		then warrior.familyOf((nox.object(h.bonus.weapon) :: any).type) or "-"
		else "-"))
	table.insert(out, "shield " .. (if h.bonus and h.bonus.shield then "yes" else "no"))
	return out
end

-- The data's names (dd check): each family's kinds some item type's (data/tips.luau's), each
-- thrown missile a thing; every skill node's skill known.
function warrior.check(bad: { string }, thing: (string, string) -> ())
	local kinds: { [string]: boolean } = {}
	for name, t in tipData.items :: any do
		if t.kind then
			kinds[t.kind] = true
			if familyOfKind[t.kind] then
				thing(name, "mastery " .. familyOfKind[t.kind])
			end
		end
	end
	for key, m in data.mastery :: any do
		for _, k in m.kinds do
			if not kinds[k] then
				table.insert(bad, "mastery " .. key .. ": no item type of kind " .. k)
			end
		end
		for _, t in m.thrown or {} do
			thing(t, "mastery " .. key .. " thrown")
		end
	end
	local tree = require("tree")
	local onTree: { [string]: boolean } = {}
	for id = 1, tree.count() do
		local n = tree.node(id)
		if n.skill and not data.numbers(n.skill, 1) then
			table.insert(bad, "tree node " .. id .. ": no skill " .. tostring(n.skill))
		elseif n.skill then
			onTree[n.skill] = true
		end
		-- (a choice of two: each the other's, upgrading the same ability)
		if n.excludes then
			local o = tree.node(n.excludes)
			if not o or o.excludes ~= id or o.upgrades ~= n.upgrades or not n.upgrades then
				table.insert(bad, "tree node " .. id .. ": its choice of two isn't a pair")
			end
		end
	end
	-- (each active on a node, its button's icon a mod picture in assets\abilities\ (a test proves
	-- it's there), and each of its nodes drawn with that picture)
	for key, a in data.actives :: any do
		if not onTree[key] then
			table.insert(bad, "active " .. key .. ": on no tree node")
		end
		if type(a.icon) ~= "string" or string.sub(a.icon, 1, 10) ~= "abilities/" then
			table.insert(bad, "active " .. key .. ": icon " .. tostring(a.icon) .. " isn't a picture in assets\\abilities")
		end
	end
	for id = 1, tree.count() do
		local n = tree.node(id)
		local a: any = n.skill and (data.actives :: any)[n.skill]
		if a and n.icon ~= a.icon then
			table.insert(bad, "tree node " .. id .. ": its picture " .. tostring(n.icon) .. " isn't its active's " .. tostring(a.icon))
		end
	end
end

-- "dd warrior": the first hero's lines; "rage N": its rage set (the bar told); "clear": its counts
-- emptied.
function warrior.console(args: { string }, h: any): { string }
	if not h then
		return { "no player" }
	end
	if args[2] == "rage" then
		local s = state(h)
		s.rage = math.clamp(tonumber(args[3]) or 0, 0, data.skills.rage.most)
		s.rageAt = nox.frame()
		sendRage(h)
		warrior.refresh(h)
		return { "rage " .. tostring(math.floor(s.rage)) }
	elseif args[2] == "clear" then
		local s = state(h)
		table.clear(s.counts)
		s.windAt = nil
		s.momentum = nil
		return { "cleared" }
	end
	return warrior.lines(h)
end

return warrior
