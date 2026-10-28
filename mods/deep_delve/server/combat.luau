--!strict
-- Damage (onDamage, an asked hook: it may change only the hit's amount, never the world).
--
--   * A hit on a player's unit from one of the mod's monsters (or something it owns: its arrow)
--     is scaled by the monster's tier and area level.
--     The player's resistance (worn affixes, its passive tree) takes its share off.
--   * A hit on one of the mod's monsters from a player (or something it owns: a missile, a
--     summoned creature) is scaled by the player's worn "damage" affixes and its passive tree.
--
-- The watches: every player's unit and every monster the mod makes carry nox.events.damage (set
-- in hero.luau and monsters.luau), so the hook hears only the hits that matter.

local hero = require("hero")
local monsters = require("monsters")

local combat = {}

-- Whoever stands behind a hit's source: the source itself, or its owner (a missile's, a summoned
-- creature's), one step up.
local function behind(source: number?): number?
	if not source then
		return nil
	end
	if monsters.info(source) or hero.byUnit(source) then
		return source
	end
	local o = nox.object(source)
	return if o and o.owner ~= 0 then o.owner else source
end

function combat.onDamage(target: number, source: number?, _weapon: number?, amount: number, _kind: number): number?
	local who = behind(source)
	if not who then
		return nil
	end
	local factor = 1
	local victim = hero.byUnit(target)
	if victim then
		factor = monsters.damageOf(who) * hero.taken(victim)
	elseif monsters.info(target) then
		local h = hero.byUnit(who)
		if h then
			factor = hero.dealt(h)
		end
	end
	if factor == 1 then
		return nil
	end
	return math.max(if amount > 0 then 1 else 0, math.floor(amount * factor + 0.5))
end

return combat
