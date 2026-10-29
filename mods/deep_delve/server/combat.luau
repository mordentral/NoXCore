--!strict
-- Damage (onDamage, an asked hook: it may change only the hit's amount, never the world).
--
--   * No damage within the party (by decision): a hit on a player's unit, or on a creature a
--     player summoned (summons.luau), made by a player or by anything of a player's (its missile,
--     its creature, its creature's missile) is cancelled (0). So players can't hurt each other or
--     anyone's creatures, their own included, and a player's creatures hurt no player and no
--     player's creature. A player's own spells on itself are the game's, but for its Fireball:
--   * No splash from a hero's own Fireball (by decision): its burst (the hit's source the missile
--     itself, the hero its owner) or the missile meeting its own hero (the weapon the missile) does
--     nothing to that hero. A Fireball turned back by a reflection is the reflector's, so it is no
--     longer the hero's own.
--   * A hit on a player's unit from one of the mod's monsters (or something it owns: its arrow)
--     is scaled by the monster's tier and area level.
--     The player's resistance (worn affixes, its passive tree) takes its share off.
--   * A hit on one of the mod's monsters from a player (or something it owns: a missile, a
--     summoned creature) is scaled by the player's worn "damage" affixes and its passive tree
--     (and a spell's own bonus when the hit came by that spell's missile: hero.dealt).
--   * Monsters hurt the players' creatures as the game has it.
--
-- The watches: every player's unit, every monster the mod makes and every creature a player
-- summons carry nox.events.damage (set in hero.luau, monsters.luau and summons.luau), so the hook
-- hears only the hits that matter. A cancelled hit is kept in a short list for "dd hits".

local hero = require("hero")
local monsters = require("monsters")
local summons = require("summons")
local numbers = require("data/numbers")

local combat = {}

local ownSpared: { [string]: boolean } = numbers.party.ownSpared :: any

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

-- The hero's unit behind a hit's source, up its owners (a missile, a summoned creature, a
-- creature's missile: at most a few steps), and whether one step was a hero's summoned creature;
-- nil when no hero is behind it.
local function heroBehind(source: number?): (number?, boolean)
	local id = source
	local viaCreature = false
	for _ = 1, 4 do
		if not id or id == 0 then
			return nil, false
		end
		if hero.byUnit(id) then
			return id, viaCreature
		end
		if summons.ownerOf(id) then
			viaCreature = true
		end
		local o = nox.object(id)
		if not o then
			return nil, false
		end
		id = o.owner
	end
	return nil, false
end

-- The Fireball (of the kinds a hero's cast makes) owned by `unit` that a hit came by, if any: the
-- burst's source, or the missile meeting it as the weapon. Its type, else nil.
local function ownFireball(unit: number, source: number?, weapon: number?): string?
	for i = 1, 2 do
		local id = if i == 1 then source else weapon
		local o = if id then nox.object(id) else nil
		if o and ownSpared[o.type] and o.owner == unit then
			return o.type
		end
	end
	return nil
end

-- The last cancelled hits, newest last (for "dd hits").
local recent: { string } = {}
local RECENT_MOST = 16

local function nameOf(id: number?): string
	if not id or id == 0 then
		return "-"
	end
	local h = hero.byUnit(id)
	if h then
		return tostring(h.record.identity.name)
	end
	local o = nox.object(id)
	if not o then
		return "?"
	end
	local owner = if o.owner ~= 0 then hero.byUnit(o.owner) else nil
	return o.type .. (if owner then "(" .. tostring(owner.record.identity.name) .. ")" else "")
end

local function note(why: string, target: number, source: number?, weapon: number?, amount: number, kind: number)
	table.insert(recent, string.format("%s: %s by %s, weapon %s, kind %d, %d cancelled", why, nameOf(target), nameOf(source), nameOf(weapon), kind, amount))
	if #recent > RECENT_MOST then
		table.remove(recent, 1)
	end
end

function combat.onDamage(target: number, source: number?, weapon: number?, amount: number, kind: number): number?
	local victim = hero.byUnit(target)
	if victim or summons.ownerOf(target) then
		-- (the party's own: a player, or a creature one summoned)
		local maker, viaCreature = heroBehind(source)
		if maker then
			local why: string? = nil
			if maker ~= target or viaCreature then
				why = (if viaCreature then "creature" else "player") .. " on " .. (if victim then "player" else "creature")
			else
				local fireball = ownFireball(target, source, weapon)
				if fireball then
					why = "own " .. fireball
				end
			end
			if why then
				note(why, target, source, weapon, amount, kind)
				return 0
			end
		end
	end
	local who = behind(source)
	if not who then
		return nil
	end
	local factor = 1
	if victim then
		factor = monsters.damageOf(who) * hero.taken(victim)
	elseif monsters.info(target) then
		local h = hero.byUnit(who)
		if h then
			factor = hero.dealt(h, weapon)
		end
	end
	if factor == 1 then
		return nil
	end
	return math.max(if amount > 0 then 1 else 0, math.floor(amount * factor + 0.5))
end

-- The last cancelled hits as lines, oldest first: "why: target by source, weapon W, kind K, N
-- cancelled" (a thing by its type, a hero's by its name in brackets).
function combat.lines(): { string }
	return table.clone(recent)
end

-- The list emptied (for "dd hits clear").
function combat.clear()
	table.clear(recent)
end

return combat
