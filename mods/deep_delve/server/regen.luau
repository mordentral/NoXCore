--!strict
-- Passive regeneration (by decision 2026-10-29): each hero gets health and mana back every second,
-- on top of the game's own refill (health and mana each from empty in 300 s, health not within a
-- second of a hurt), at its class's base rate (data/numbers.luau's regen) plus what its passive
-- tree adds (the healthRegen and manaRegen stats, per second).
--
-- A timer per hero, every numbers.regen.every frames (a few times a second); nothing walks the
-- world. The share of a point each beat is worth is kept until it makes a whole point. Nothing
-- comes back while the hero is dead or between maps (not set up), and nothing carries over that
-- pause. Health waits numbers.regen.afterHurt frames after a hit the hero took (heard by its
-- unit's damaged event), as the game's own refill does; mana doesn't wait.

local numbers = require("data/numbers")

local regen = {}

-- A hero's regeneration a second, from the mod (its class's base and its tree): health, mana. A
-- class without mana gets none.
function regen.rates(h: any): (number, number)
	local r = numbers.regen
	local base = (r.base :: any)[h.record.identity.class] or r.base.warrior
	local b = h.bonus or {}
	local health = math.max(0, base.health + (b.healthRegen or 0))
	local mana = math.max(0, base.mana + (b.manaRegen or 0))
	return health, mana
end

local function beat(h: any)
	local s = h.regen
	if not s then
		return
	end
	if not h.ready or not nox.living(h.unit) then
		-- (paused: dead, or between maps)
		s.health, s.mana = 0, 0
		return
	end
	local o = nox.object(h.unit)
	if not o then
		return
	end
	local every = numbers.regen.every
	local healthRate, manaRate = regen.rates(h)
	if healthRate > 0 and o.health < o.maxHealth and nox.frame() - s.hurtAt >= numbers.regen.afterHurt then
		s.health += healthRate * every / 30
		local whole = math.floor(s.health)
		if whole >= 1 then
			s.health -= whole
			nox.heal(h.unit, math.min(whole, o.maxHealth - o.health))
		end
	else
		s.health = 0
	end
	if manaRate > 0 then
		local mana, most = nox.mana(h.unit)
		if mana < most then
			s.mana += manaRate * every / 30
			local whole = math.floor(s.mana)
			if whole >= 1 then
				s.mana -= whole
				nox.setMana(h.unit, math.min(most, mana + whole), most, "plain")
			end
		else
			s.mana = 0
		end
	end
end

-- The hero's timer started (its join; one already running is replaced).
function regen.start(h: any)
	regen.stop(h)
	h.regen = { timer = nox.every(numbers.regen.every, beat, h), health = 0, mana = 0, hurtAt = -100000 }
end

-- The hero's timer stopped (it left).
function regen.stop(h: any)
	local s = h.regen
	if s then
		nox.cancelTimer(s.timer)
		h.regen = nil
	end
end

-- What "dd regen" says of a hero: its rates a second ("rates H M"), and its state now: "paused"
-- (dead or between maps), "waiting" (health held after a hit), "running" or "stopped" (no timer).
function regen.lines(h: any): { string }
	local health, mana = regen.rates(h)
	local s = h.regen
	local state = "running"
	if not s then
		state = "stopped"
	elseif not h.ready or not nox.living(h.unit) then
		state = "paused"
	elseif nox.frame() - s.hurtAt < numbers.regen.afterHurt then
		state = "waiting"
	end
	return { string.format("rates %.1f %.1f", health, mana), state }
end

-- The hero took a hit of `amount`: its health's regeneration waits.
function regen.hurt(h: any, amount: number)
	if amount > 0 and h.regen then
		h.regen.hurtAt = nox.frame()
	end
end

return regen
