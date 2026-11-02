--!strict
-- The worn uniques' powers and the set bonuses' powers at work (data/uniques.luau's numbers). A
-- hero's bonus carries `powers` (a kind -> its numbers: uniques.worn), worked out again at each
-- set-up and equip; each power is looked up there by its kind, never by a walk.
--
-- By the hooks they need:
--   a hero's hit on one of the mod's monsters (combat.luau, uniquePowers.dealt): frenzy raises
--     the hit; leech heals by it; on the worn weapon's own hit (its blow, or its bow's arrow, by
--     onDamage's cause) crit may double it and chain (Chain Lightning) and freeze (the game's
--     hold) may go off; seeker may loose another arrow at the next foe. What changes the world is
--     done a frame on (an asked hook changes nothing);
--   a hit on the hero (combat.luau, uniquePowers.taken): bulwark caps it, manaShield pays a share
--     from mana, undying leaves 1 health (once in a while) and a ring of fire, thorns deals a share
--     of a monster's own blow (the cause "blow") back to it;
--   a kill (main.luau's onObjectKilled, uniquePowers.killed): explode bursts the monster,
--     healOnKill heals, frenzy counts on;
--   a cast (onSpellCast, uniquePowers.cast): split throws more Fireballs beside one, echo casts
--     every Nth spell again;
--   a price (castCost, uniquePowers.cost): bloodMagic pays it in health;
--   a creature found (summons.luau): packVamp puts the game's vampirism on it;
--   time (a timer per hero wearing one, data/uniques.luau's beat): trail casts the game's Burn
--     where the hero has walked (its flames spare their own hero), nova strikes the monsters near
--     it, lastWind hastes it below a share of its health, companion keeps a creature at its side.
--
-- The party rule holds: the powers' hits come from the hero, and combat.luau cancels any hit
-- within the party first; the mod's monsters never hurt each other. Damage a power deals itself
-- (nova, a burst, thorns) is `quiet`: no power rolls on it again.

local data = require("data/uniques")
local affixes = require("affixes")
local casts = require("casts")
local hero = require("hero")
local monsters = require("monsters")
local summons = require("summons")

local uniquePowers = {}

-- The kinds built here (uniques.check holds the data to them).
uniquePowers.KINDS = {
	leech = true, thorns = true, chain = true, trail = true, explode = true, bulwark = true, lastWind = true,
	split = true, manaShield = true, echo = true, undying = true, bloodMagic = true, nova = true,
	companion = true, seeker = true, packVamp = true, freeze = true, crit = true, frenzy = true, healOnKill = true,
}

local FPS = 30
-- How long undying's ring of fire is kept spared for its hero (frames: past its flames' end).
local RING_FRAMES = 20 * FPS
local MONSTERS: any = { monsters = true, alive = true }

-- Damage a power deals itself: no power rolls on it.
local quiet = false
-- A cast a power makes (split, echo): not heard as the hero's own again.
local casting = false
-- The monsters a burst hit (explode): their deaths burst no more.
local burstHit: { [number]: boolean } = {}
local burstCount = 0
-- The things a power made that spare their own hero (trail's flames, undying's ring): id -> its
-- hero's unit.
local spareOf: { [number]: number } = {}
-- The arrows seeker loosed (none of them looses another).
local seekers: { [number]: boolean } = {}
local seekerCount = 0

-- A hero's power (its numbers), or nil.
local function power(h: any, kind: string): any
	local p = h.bonus and h.bonus.powers
	return p and p[kind]
end

-- A hero's own state for its uniquePowers.
local function state(h: any): any
	local s = h.powerState
	if not s then
		s = { counts = {}, flames = {} }
		h.powerState = s
	end
	return s
end

local function count(h: any, what: string)
	local c = state(h).counts
	c[what] = (c[what] or 0) + 1
end

-- The things made since a mark that spare their own hero.
local function spareMade(unit: number, mark: any, list: { number }?)
	for _, id in nox.madeSince(mark) do
		spareOf[id] = unit
		if list then
			table.insert(list, id)
		end
	end
end

-- ---- A hero's hit on one of the mod's monsters (combat.luau)

-- The hit's extra factor (1: none); `factor` what the hit is scaled by already, `cause`
-- onDamage's (the weapon's hits: its blow, or its bow's own arrow: affixes.byWeapon).
function uniquePowers.dealt(h: any, target: number, m: any, weapon: number?, kind: number, amount: number, factor: number, cause: string): number
	if quiet then
		-- (a power's own hit: the monster's death from it bursts no more; the list let go now and
		-- then, its older entries long past)
		if burstCount >= 400 then
			table.clear(burstHit)
			burstCount = 0
		end
		if not burstHit[target] then
			burstHit[target] = true
			burstCount += 1
		end
		return 1
	end
	local p = h.bonus and h.bonus.powers
	if not p or next(p) == nil then
		return 1
	end
	local mult = 1
	local now = nox.frame()
	local fr = p.frenzy
	local s = state(h)
	if fr and s.frenzy and now - s.frenzy.at <= fr.seconds * FPS then
		mult *= 1 + s.frenzy.n * fr.step / 100
	end
	local unit = h.unit
	local byWeapon = affixes.byWeapon(h.bonus, unit, weapon, cause)
	if byWeapon and p.crit and nox.random() * 100 < p.crit.chance then
		mult *= p.crit.times
		count(h, "crit")
	end
	if byWeapon then
		local chain = p.chain
		if chain and nox.random() * 100 < chain.chance then
			count(h, "chain")
			nox.after(1, function()
				if nox.living(target) and nox.present(unit) then
					local x, y = nox.position(target)
					nox.cast(chain.spell, unit, target, x, y, chain.level)
				end
			end)
		end
		local fz = p.freeze
		if fz and m.tier ~= "boss" and nox.random() * 100 < fz.chance then
			count(h, "freeze")
			nox.after(1, function()
				if nox.living(target) then
					nox.enchant(target, "ENCHANT_HELD", fz.frames, 1)
				end
			end)
		end
	end
	local sk = p.seeker
	if sk and cause == "missile" and weapon and not seekers[weapon] and h.bonus.ammo then
		local o = nox.object(weapon)
		if o and o.type == h.bonus.ammo and o.owner == unit and nox.random() * 100 < sk.chance then
			local ammo = o.type
			nox.after(1, function()
				uniquePowers.seek(h, target, ammo, sk.reach)
			end)
		end
	end
	local lp = p.leech
	if lp and amount > 0 then
		s.heal = (s.heal or 0) + amount * factor * mult * lp.pct / 100
		if not s.healDue then
			s.healDue = true
			nox.after(1, function()
				s.healDue = nil
				local whole = math.floor(s.heal or 0)
				s.heal = (s.heal or 0) - whole
				if whole >= 1 and nox.living(unit) then
					nox.heal(unit, whole)
				end
			end)
		end
	end
	return mult
end

-- Seeker: another arrow from the hero at the nearest foe it sees other than the one struck.
function uniquePowers.seek(h: any, struck: number, ammo: string, reach: number)
	if not nox.living(h.unit) then
		return
	end
	for _, foe in nox.nearestFoes(h.unit, reach, 4, 4) do
		if foe ~= struck and monsters.info(foe) then
			local x, y = nox.position(h.unit)
			local fx, fy = nox.position(foe)
			local id = nox.shoot(ammo, h.unit, nox.directionOf(fx - x, fy - y))
			if id then
				if seekerCount > 100 then
					-- (the gone ones let go)
					local kept: { [number]: boolean } = {}
					seekerCount = 0
					for a in seekers do
						if nox.present(a) then
							kept[a] = true
							seekerCount += 1
						end
					end
					seekers = kept
				end
				seekers[id] = true
				seekerCount += 1
				count(h, "seeker")
			end
			return
		end
	end
end

-- ---- A hit on the hero (combat.luau): its new amount, or nil (as it was)

function uniquePowers.taken(h: any, who: number?, source: number?, weapon: number?, amount: number, kind: number, cause: string): number?
	local p = h.bonus and h.bonus.powers
	if quiet or not p or next(p) == nil or amount <= 0 then
		return nil
	end
	local o = nox.object(h.unit)
	if not o then
		return nil
	end
	local unit = h.unit
	local s = state(h)
	local out = amount
	local bw = p.bulwark
	if bw then
		out = math.min(out, math.max(1, math.floor(o.maxHealth * bw.pct / 100)))
	end
	local ms = p.manaShield
	if ms and out > 0 then
		local mana = nox.mana(unit)
		local owed = s.manaOwed or 0
		local paid = math.min(math.floor(out * ms.share / 100 + 0.5), mana - owed)
		if paid > 0 then
			out -= paid
			s.manaOwed = owed + paid
			count(h, "shield")
			if not s.manaDue then
				s.manaDue = true
				nox.after(1, function()
					s.manaDue = nil
					local now, most = nox.mana(unit)
					nox.setMana(unit, math.max(0, now - (s.manaOwed or 0)), most, "plain")
					s.manaOwed = 0
				end)
			end
		end
	end
	local th = p.thorns
	-- (thorns: a monster's own blow, by onDamage's cause)
	if th and cause == "blow" and who and who == source and monsters.info(who) then
		local back = math.floor(amount * th.pct / 100 + 0.5)
		if back > 0 then
			count(h, "thorns")
			nox.after(1, function()
				if nox.living(who) and nox.present(unit) then
					quiet = true
					pcall(nox.damage, who, unit, back, 0)
					quiet = false
				end
			end)
		end
	end
	local ud = p.undying
	if ud and out >= o.health and nox.frame() >= (s.undyingAt or 0) then
		out = math.max(0, o.health - 1)
		s.undyingAt = nox.frame() + ud.seconds * FPS
		count(h, "undying")
		nox.after(1, function()
			if nox.living(unit) then
				local x, y = nox.position(unit)
				local mark = nox.newMark()
				nox.cast(ud.spell, unit, nil, x, y, ud.level)
				local ring = {}
				spareMade(unit, mark, ring)
				nox.tell(unit, "The last ember holds you to life.")
				-- (the ring's flames let go once they're long out)
				nox.after(RING_FRAMES, function()
					for _, id in ring do
						if spareOf[id] == unit then
							spareOf[id] = nil
						end
					end
				end)
			end
		end)
	end
	return if out ~= amount then out else nil
end

-- Whether a hit on `target` comes by a thing a power made that spares its own hero (trail's
-- flames, undying's ring): cancelled.
function uniquePowers.spares(target: number, source: number?, weapon: number?): boolean
	return (source ~= nil and spareOf[source] == target) or (weapon ~= nil and spareOf[weapon] == target)
end

-- ---- A kill (main.luau's onObjectKilled: `m` the mod's monster's entry)

-- The hero whose kill it was: its own unit, or a thing it owns (its missile), not its creature's.
local function killerOf(by: number?): any
	local id = by
	for _ = 1, 3 do
		if not id or id == 0 then
			return nil
		end
		local h = hero.byUnit(id)
		if h then
			return h
		end
		if summons.ownerOf(id) then
			return nil
		end
		local o = nox.object(id)
		if not o then
			return nil
		end
		id = o.owner
	end
	return nil
end

-- Bursts this frame (explode's most).
local burstFrame = -1
local burstsNow = 0

function uniquePowers.killed(m: any, e: any)
	local fromBurst = burstHit[e.id] == true
	if fromBurst then
		burstHit[e.id] = nil
		burstCount -= 1
	end
	local h = killerOf(e.by)
	if not h then
		return
	end
	local p = h.bonus and h.bonus.powers
	if not p or next(p) == nil then
		return
	end
	local now = nox.frame()
	local ex = p.explode
	if ex and not fromBurst then
		if burstFrame ~= now then
			burstFrame, burstsNow = now, 0
		end
		if burstsNow < ex.most then
			burstsNow += 1
			count(h, "explode")
			local damage = math.max(1, math.floor((m.base or 10) * ex.pct / 100 + 0.5))
			nox.effect(0x85, e.x, e.y) -- (MSG_FX_EXPLOSION)
			quiet = true
			local ok, err = pcall(nox.damageAt, e.x, e.y, ex.reach, ex.reach // 2, damage, 7, h.unit, nil, true)
			quiet = false
			if not ok then
				nox.log("deep_delve: a burst not dealt: " .. tostring(err))
			end
		end
	end
	local hk = p.healOnKill
	if hk and nox.living(h.unit) then
		local o = nox.object(h.unit)
		if o and o.health < o.maxHealth then
			nox.heal(h.unit, math.max(1, math.floor(o.maxHealth * hk.pct / 100)))
		end
	end
	local fr = p.frenzy
	if fr then
		local s = state(h)
		local f = s.frenzy
		if f and now - f.at <= fr.seconds * FPS then
			f.n = math.min(f.n + 1, math.floor(fr.most / fr.step))
			f.at = now
		else
			s.frenzy = { n = 1, at = now }
		end
	end
end

-- ---- A cast (onSpellCast)

-- The point a cast went at: its target's, else its own.
local function aimOf(target: number?, x: number, y: number): (number, number)
	if target and nox.present(target) then
		local tx, ty = nox.position(target)
		return tx, ty
	end
	return x, y
end

function uniquePowers.cast(caster: number, spell: any, target: number?, x: number, y: number)
	if casting or type(spell) ~= "string" then
		return
	end
	local h = hero.byUnit(caster)
	local p = h and h.bonus and h.bonus.powers
	if not h or not p or next(p) == nil or not nox.present(caster) then
		return
	end
	local level = math.clamp(nox.spellLevel(caster, spell), 1, 5)
	local sp = p.split
	if sp and spell == "SPELL_FIREBALL" then
		local cx, cy = nox.position(caster)
		local ax, ay = aimOf(target, x, y)
		local dx, dy = ax - cx, ay - cy
		local dist = math.max(80, math.sqrt(dx * dx + dy * dy))
		local angle = math.atan2(dy, dx)
		casting = true
		for i = 1, sp.extra do
			local side = if i % 2 == 1 then 1 else -1
			local a = angle + side * math.ceil(i / 2) * sp.spread * math.pi / 128
			local mark = nox.newMark()
			if nox.cast("SPELL_FIREBALL", caster, nil, cx + dist * math.cos(a), cy + dist * math.sin(a), level) then
				count(h, "split")
				-- (sized as the hero's own Fireball at its level: casts.luau)
				casts.cast(caster, "SPELL_FIREBALL", nox.madeSince(mark))
			end
		end
		casting = false
	end
	local ec = p.echo
	if ec and table.find(ec.spells, spell) then
		local s = state(h)
		s.echo = (s.echo or 0) + 1
		if s.echo >= ec.n then
			s.echo = 0
			local ax, ay = aimOf(target, x, y)
			casting = true
			local mark = nox.newMark()
			if nox.cast(spell, caster, if target and nox.present(target) then target else nil, ax, ay, level) then
				count(h, "echo")
				casts.cast(caster, spell, nox.madeSince(mark))
			else
				count(h, "echoRefused")
			end
			casting = false
		end
	end
end

-- ---- A price (castCost): bloodMagic's in health; nil: as it was

function uniquePowers.cost(caster: number, mana: number, health: number?): (number?, number?)
	local h = hero.byUnit(caster)
	local bm = h and power(h, "bloodMagic")
	if not bm or mana <= 0 then
		return nil, nil
	end
	return 0, (health or 0) + math.max(1, math.floor(mana * bm.pct / 100 + 0.5))
end

-- ---- A creature found (summons.luau)

local VAMP = "ENCHANT_VAMPIRISM"

local function found(id: number, owner: number)
	local h = hero.byUnit(owner)
	if h and power(h, "packVamp") and nox.living(id) then
		nox.enchant(id, VAMP, 0)
	end
end
summons.onFound(found)

-- ---- Time: the hero's beat

local function companionGone(s: any)
	local c = s.companion
	if c and nox.present(c) then
		nox.delete(c)
	end
	s.companion = nil
	s.companionAt = nil
end

local function trail(h: any, s: any, tp: any, x: number, y: number, now: number)
	if not s.trailX then
		s.trailX, s.trailY = x, y
	end
	local dx, dy = x - s.trailX, y - s.trailY
	if dx * dx + dy * dy >= tp.step * tp.step then
		local mark = nox.newMark()
		if nox.cast(tp.spell, h.unit, nil, s.trailX, s.trailY, tp.level) then
			local made = {}
			spareMade(h.unit, mark, made)
			for _, id in made do
				table.insert(s.flames, { id = id, ends = now + tp.seconds * FPS })
			end
			count(h, "trail")
		end
		s.trailX, s.trailY = x, y
	end
end

-- Trail's flames past their time put out (each hero's own list).
local function putOut(s: any, now: number, all: boolean)
	local list = s.flames
	for i = #list, 1, -1 do
		local f = list[i]
		if all or now >= f.ends or not nox.present(f.id) then
			if nox.present(f.id) then
				nox.delete(f.id)
			end
			spareOf[f.id] = nil
			table.remove(list, i)
		end
	end
end

local function beat(h: any)
	local s = state(h)
	local p = h.bonus and h.bonus.powers or {}
	local now = nox.frame()
	putOut(s, now, false)
	if not h.ready or not nox.living(h.unit) then
		s.trailX = nil
		return
	end
	local x, y = nox.position(h.unit)
	local tp = p.trail
	if tp then
		trail(h, s, tp, x, y, now)
	else
		s.trailX = nil
	end
	local nv = p.nova
	if nv and now >= (s.novaAt or 0) then
		local filter: any = { monsters = true, alive = true, enemiesOf = h.unit }
		if nox.countNear(x, y, nv.reach, filter) > 0 then
			s.novaAt = now + nv.seconds * FPS
			local level = h.record.level.level
			count(h, "nova")
			nox.effect(0x87, x, y) -- (MSG_FX_COUNTERSPELL_EXPLOSION)
			quiet = true
			pcall(nox.damageAt, x, y, nv.reach, nv.reach // 2, nv.damage + nv.perLevel * (level - 1), 9, h.unit, nil, true)
			quiet = false
		end
	end
	local lw = p.lastWind
	if lw then
		local o = nox.object(h.unit)
		if o and o.health * 100 < o.maxHealth * lw.below then
			local left = nox.enchantTime(h.unit, "ENCHANT_HASTED")
			local on = nox.enchanted(h.unit, "ENCHANT_HASTED")
			if not on or (left > 0 and left < data.beat.every * 2) then
				nox.enchant(h.unit, "ENCHANT_HASTED", data.beat.every * 4)
				count(h, "lastWind")
			end
		end
	end
	local cp = p.companion
	if cp then
		if not s.companion or not nox.living(s.companion) then
			s.companion = nil
			if not s.companionAt then
				-- (the first at once; after a fall, its time on)
				s.companionAt = if s.hadCompanion then now + cp.seconds * FPS else now
			end
			if now >= s.companionAt then
				local px, py = nox.clearPointNear(x + 30, y, 30)
				local id = nox.summon(cp.creature, px, py, h.unit, nox.direction(h.unit))
				if id then
					s.companion = id
					s.companionAt = nil
					s.hadCompanion = true
					summons.adopt(id, h.unit)
					count(h, "companion")
				end
			end
		end
	end
end

-- ---- What the hero wears changed (hero.onBonus): its timer, its companion, its creatures'
-- vampirism.

local function timed(p: any): boolean
	return p.trail ~= nil or p.nova ~= nil or p.lastWind ~= nil or p.companion ~= nil
end

function uniquePowers.refresh(h: any)
	local p = h.bonus and h.bonus.powers or {}
	local s = state(h)
	local vamp = p.packVamp ~= nil
	if vamp ~= (s.vamp == true) then
		s.vamp = vamp
		for _, id in summons.of(h.unit) do
			if vamp then
				nox.enchant(id, VAMP, 0)
			elseif nox.enchanted(id, VAMP) then
				nox.enchant(id, VAMP, -1)
			end
		end
	end
	if not p.companion and s.companion then
		companionGone(s)
		s.hadCompanion = nil
	end
	if not p.trail and #s.flames > 0 then
		putOut(s, nox.frame(), true)
	end
	if timed(p) and not s.beat then
		local peer = h.peer
		local timer: number = 0
		timer = nox.every(data.beat.every, function()
			if hero.byPeer(peer) ~= h or s.beat ~= timer then
				-- (the hero gone, or another in its place: its beat ends, its companion and flames with it)
				nox.cancelTimer(timer)
				if s.beat == timer then
					s.beat = nil
				end
				companionGone(s)
				putOut(s, nox.frame(), true)
				return
			end
			beat(h)
		end)
		s.beat = timer
	elseif not timed(p) and s.beat then
		nox.cancelTimer(s.beat)
		s.beat = nil
	end
end
hero.onBonus(uniquePowers.refresh)

-- A new map: the old map's things (flames, companions) went with it.
function uniquePowers.reset()
	table.clear(spareOf)
	table.clear(burstHit)
	burstCount = 0
	table.clear(seekers)
	seekerCount = 0
	for _, h in hero.all() do
		local s = h.powerState
		if s then
			s.companion = nil
			s.companionAt = nil
			s.hadCompanion = nil -- (a new one at once in the new map)
			s.trailX = nil
			table.clear(s.flames)
		end
	end
end

-- What "dd powers" says of a hero: each power worn ("power KIND"), then its counts ("count WHAT N",
-- how often each went off) and its state (frenzy's stacks, undying's wait, the companion, the
-- flames burning).
function uniquePowers.lines(h: any): { string }
	local out = {}
	local p = h.bonus and h.bonus.powers or {}
	local kinds = {}
	for kind in p do
		table.insert(kinds, kind)
	end
	table.sort(kinds)
	for _, kind in kinds do
		table.insert(out, "power " .. kind)
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
	if s.frenzy then
		table.insert(out, string.format("frenzy %d %d", s.frenzy.n, now - s.frenzy.at))
	end
	if s.undyingAt then
		table.insert(out, "undying " .. tostring(math.max(0, s.undyingAt - now)))
	end
	table.insert(out, "companion " .. tostring(s.companion or "-"))
	table.insert(out, "flames " .. tostring(#s.flames))
	table.insert(out, "beat " .. (if s.beat then "on" else "off"))
	return out
end

return uniquePowers
