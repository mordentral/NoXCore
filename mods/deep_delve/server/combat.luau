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
--     is scaled by the monster's tier and area level; one by something it owns, times its
--     `spells` too (a boss's: the barrow-king's mist and fists; 1 for every other monster).
--     The player's resistance (worn affixes, its passive tree) takes its share off, and its
--     shield's guard its share of a blow or missile from a monster in front of it.
--   * The mod's monsters never hurt each other (a hit on one by another, or by something another
--     owns, is cancelled): a boss's quake, mist or fist spares its own dead.
--   * A hit on one of the mod's monsters from a player (or something it owns: a missile, a
--     summoned creature) is scaled by the player's worn "damage" affixes and its passive tree
--     (and a spell's own bonus when the hit came by that spell's missile: hero.dealt), and by the
--     share of a blow the monster takes (its entry's `taken`: the ruin-warden's plates, act 4's
--     boss, warden.luau; none: all of it).
--   * Monsters hurt the players' creatures as the game has it.
--   * The worn affixes (affixes.luau): a hit on a monster by the hero's worn weapon (its blow or
--     its bow's arrow, by the hit's cause) is up by its grip and may chill (frost) or frighten
--     (dread) the monster. A monster's missile about to hit a hero may be turned (reflect,
--     onReflect: the game turns it back, the hero's now); its hits are then dealt as its shooter's
--     would have been on the hero, no bonus of the hero's on them. A spell's hit on a hero by a
--     thing a monster owns with no missile (a boss's mist or fist) may be turned back too: cancelled
--     here, its damage dealt to the monster a frame on. A weapon's hit has its wear looked at a
--     frame on (resilience).
--   * A hit on a hero by a monster with powers (powers.luau) is told to it as it lands (a
--     frostbound one's slow, a vampiric one's heal: the next frame). Nothing about the hit changes.
--   * The worn uniques' and sets' powers (uniquepowers.luau): the hero's hit on a monster may be
--     raised (crit, frenzy) and set off what goes with it (leech, Chain Lightning, a hold, another
--     arrow; the weapon's hits by the hit's cause); a hit on the hero may be capped (bulwark),
--     paid partly from mana, survived at 1 health (undying) or answered (thorns: a monster's own
--     blow). A flame a power made never hurts its own hero.
--   * The warrior's passive skills (warrior.luau): its blow may be raised (Momentum, Execute,
--     Ambush) and followed a frame on by what goes with it (Cleave, the bleeds, a shockwave, a
--     push, the rage's spin), any party member's hit by Hunter's Mark; a monster's blow on it may
--     be parried (0) or answered by a stagger. A skill's own hit is dealt as it is.
--   * A flame hit on the troll chieftain (act 3's boss: mosschief.luau) is noted on him, with the
--     hero behind it: fire keeps his wounds from closing. Nothing about the hit changes.
--
-- The watches: every player's unit, every monster the mod makes and every creature a player
-- summons carry nox.events.damage (set in hero.luau, monsters.luau and summons.luau), so the hook
-- hears only the hits that matter. A cancelled hit is kept in a short list for "dd hits".

local hero = require("hero")
local monsters = require("monsters")
local summons = require("summons")
local mosschief = require("mosschief")
local affixes = require("affixes")
local powers = require("powers")
local uniquePowers = require("uniquepowers")
local warrior = require("warrior")
local numbers = require("data/numbers")
local lootData = require("data/loot")

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

-- A hit turned back on the monster `who` (a frame after it was cancelled): the engine's damage from
-- the hero, the hero's damage bonuses no part of it (`reflecting`).
local reflecting = false
local turned = 0

local function turnBack(who: number, unit: number, amount: number, kind: number)
	if amount <= 0 or not nox.living(who) or not monsters.info(who) or not nox.present(unit) then
		return
	end
	reflecting = true
	local ok, err = pcall(function()
		nox.damage(who, unit, amount, kind)
	end)
	reflecting = false
	if not ok then
		nox.log("deep_delve: a hit not turned back: " .. tostring(err))
	end
end

-- How many hits were turned back since the start (for "dd hits").
function combat.turned(): number
	return turned
end

-- The monsters' missiles a hero's reflection turned: missile id -> { hero (its owner now), scale
-- (its shooter's on a hero) }, each kept loot.reflect.keep frames.
local turnedMissiles: { [number]: any } = {}

-- The turned missile a hit came by (the weapon, or the source: a burst), while still the hero's.
local function turnedBy(source: number?, weapon: number?): any
	for i = 1, 2 do
		local id = if i == 1 then weapon else source
		local t = if id then turnedMissiles[id] else nil
		if t then
			local o = nox.object(id :: number)
			if o and o.owner == t.hero then
				return t
			end
		end
	end
	return nil
end

-- A missile about to hit a watched thing (onReflect, an asked hook): a hero's reflection may turn
-- one a monster owns. Never a party member's (no monster's), never on a monster.
function combat.onReflect(target: number, missile: number, _front: boolean): boolean
	local victim = hero.byUnit(target)
	if not victim or not victim.bonus then
		return false
	end
	local o = nox.object(missile)
	local who = if o then o.owner else 0
	if who == 0 or not monsters.info(who) or not affixes.reflects(victim.bonus) then
		return false
	end
	note("turned back", target, who, missile, 0, 0)
	turned += 1
	local rec = { hero = target, scale = monsters.damageOf(who) * monsters.spellsOf(who) }
	turnedMissiles[missile] = rec
	nox.after(lootData.reflect.keep, function()
		if turnedMissiles[missile] == rec then
			turnedMissiles[missile] = nil
		end
	end)
	return true
end

function combat.onDamage(target: number, source: number?, weapon: number?, amount: number, kind: number, cause: string): number?
	-- (a power's own flame on its own hero: none, uniquepowers.luau)
	if uniquePowers.spares(target, source, weapon) then
		return 0
	end
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
	-- (fire on the troll chieftain keeps his wounds open: a flame hit from anything noted on him,
	-- with the hero behind it; noted only, the hit goes on as it would)
	if kind == mosschief.FLAME and not victim then
		local m = monsters.info(target)
		if m and m.scorchable then
			mosschief.burnt(m, (heroBehind(source)))
		end
	end
	local who = behind(source)
	-- (one of the mod's monsters struck with a hero behind the blow: its pack wakes to whoever
	-- struck, monsters.struck; nothing about the hit changes)
	if not victim and monsters.info(target) then
		local maker = heroBehind(source)
		if maker then
			monsters.struck(target, if source and source ~= maker then { maker, source } else { maker })
		end
	end
	if not who then
		-- (a hit from nothing, a trap's or poison's: only the hero's worn powers on it)
		return if victim then uniquePowers.taken(victim, nil, source, weapon, amount, kind, cause) else nil
	end
	-- (the mod's monsters never hurt each other: a boss's own quake, mist or fist among its dead)
	if not victim and monsters.info(target) and monsters.info(who) then
		return 0
	end
	local factor = 1
	local warriorHit = false
	if victim then
		local scaled = monsters.damageOf(who)
		-- (a thing it owns, not its own blow: a boss's spells, times its own share)
		if who ~= source then
			scaled *= monsters.spellsOf(who)
		end
		-- (the hero's reflection on a spell's thing with no missile, a boss's mist or fist: turned
		-- back, its damage as the hero would have had it before its resistance dealt to the monster
		-- a frame on; a missile is turned by onReflect before it hits)
		if cause == "spell" and monsters.info(who) and affixes.turnsBack(victim.bonus, who, weapon) then
			local back = math.max(if amount > 0 then 1 else 0, math.floor(amount * scaled + 0.5))
			note("turned back", target, source, weapon, amount, kind)
			turned += 1
			local unit = victim.unit
			nox.after(1, function()
				turnBack(who, unit, back, kind)
			end)
			return 0
		end
		-- (its shield's guard: a blow or missile from a monster in front of it, less; affixes.luau)
		factor = scaled * hero.taken(victim) * affixes.guarded(victim.bonus, target, if monsters.info(who) then who else nil, cause)
	elseif reflecting or warrior.quiet() then
		-- (a hit turned back, or a warrior's skill's own hit: dealt as it was, the hero's bonuses no
		-- part of it again)
		return nil
	else
		local m = monsters.info(target)
		local h = if m then hero.byUnit(who) else nil
		local t = if h then turnedBy(source, weapon) else nil
		if t then
			-- (a monster's missile a hero's reflection turned: dealt as its shooter's would have been
			-- on the hero, the hero's bonuses no part of it)
			factor = t.scale
		elseif m and h then
			factor = hero.dealt(h, weapon)
			-- (the worn uniques' and sets' powers on the hero's hit: uniquepowers.luau)
			factor *= uniquePowers.dealt(h, target, m, weapon, kind, amount, factor, cause)
			-- (the warrior's skills on the hit, and Hunter's Mark on any of the party's: warrior.luau)
			factor *= warrior.dealt(h, target, m, source, weapon, amount, factor, cause, kind)
			warriorHit = true
			-- (the worn weapon's own hit: its grip, its frost's and dread's chances)
			if affixes.byWeapon(h.bonus, h.unit, weapon, cause) then
				factor += (h.bonus.grip or 0) / 100
				affixes.onWeaponHit(h.bonus, target, m)
				-- (the blow wears the weapon: its resilience gives a share back a frame on)
				affixes.worn(h)
			end
			-- (a monster that takes only a share of a blow, or more: the ruin-warden's plates and
			-- his open moments, its entry's `taken`)
			local taken = monsters.info(target).taken
			if type(taken) == "number" then
				factor *= taken
			end
		end
	end
	local final = if factor == 1 then amount else math.max(if amount > 0 then 1 else 0, math.floor(amount * factor + 0.5))
	local changed = factor ~= 1
	-- (what follows a warrior's blow, by what it dealt: warrior.luau)
	if warriorHit then
		warrior.landed(final)
	end
	-- (the hero's worn powers on the hit it takes: uniquepowers.luau; then its skills: a parry, a
	-- stagger, its rage, warrior.luau)
	if victim then
		local worn = uniquePowers.taken(victim, who, source, weapon, final, kind, cause)
		if worn then
			final, changed = worn, true
		end
		local skilled = warrior.taken(victim, who, source, weapon, final, cause)
		if skilled then
			final, changed = skilled, true
		end
	end
	-- (a powered monster's hit on a hero: its frost or its thirst follows, powers.luau)
	if victim then
		local m = monsters.info(who)
		if m and m.powers then
			powers.struck(m, who, target, final)
		end
	end
	if not changed then
		return nil
	end
	return final
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
