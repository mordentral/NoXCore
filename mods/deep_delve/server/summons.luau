--!strict
-- The heroes' creatures (a conjurer's Beasts, and what it charms): found as each one comes, and
-- watched for the hits they take (combat.luau keeps the party from hurting them) and for their
-- deaths.
--
--   * A summoning takes a while: its creature comes on a later frame than the cast, within 50 of
--     its caster. So a hero's summon cast (onSpellCast) leaves a look-out at the caster's spot,
--     and a beat looks round each look-out a few times a second (nox.unitsNear) for a creature of
--     the caster's, of the spell's type, not known yet, until the longest summoning's time has
--     passed (the game's SummonDuration for a large creature, and a little more).
--   * A charm (Charm Creature) takes a while too: the creature nearest the cast's point within 300
--     is held for the game's charm time (by its size and the spell's level), then becomes its
--     caster's (its owner), unless the caster strays past 300 first. So a hero's charm leaves a
--     look-out on the caster (wherever it walks), looking within CHARM_REACH for one of the mod's
--     monsters that has become the caster's, until the longest charm's time has passed. Found, it
--     is no longer one of the mod's monsters (monsters.forget: no experience or loot when it
--     dies, not scaled with the party), and it counts as its charmer's like a summoned creature.
--   * Known: by id, its owner's unit. An id counts only while the thing under it still has that
--     owner (nox.object), so a stale id is never taken for a creature of the party's.
--   * Dropped at its death (watched) and with the map (a new map has none of the old things).
--   * Found, it goes on the party's team (team.luau), if the engine hasn't put it there already
--     with its owner (a charm takes a creature off any team first).
--   * A summoned creature (not a charmed one) of a hero whose tree has summonHealth (a keystone)
--     has its most health changed by that share as it's found.
--
-- Nothing walks the world: only the units near a summoning or a charm while its creature is on
-- its way.

local hero = require("hero")
local monsters = require("monsters")
local team = require("team")
local treeData = require("data/tree")

local summons = {}

-- Known creatures: id -> its owner's unit.
local known: { [number]: number } = {}
-- Summonings and charms on their way: { caster, x, y, type, ends } (a charm: charm = true, no
-- type; it looks round its caster).
local coming: { any } = {}
local beat: number? = nil

-- Each summon spell's creature type (the tree's creatures, by their spell's name).
local typeOf: { [string]: string } = {}
for name, c in (treeData.creatures :: any) do
	typeOf[c.spell] = name
end

-- How far round the caster's spot to look (a creature comes within 50 of it), how often, and how
-- long past the longest summoning. A charmed creature is within 300 of its charmer's edge (else
-- the charm breaks), so CHARM_REACH round the charmer.
local LOOK_REACH = 120
local CHARM_REACH = 400
local LOOK_EVERY = 5
local LOOK_SLACK = 30
local longest: number? = nil

local function waitFrames(): number
	if not longest then
		longest = (nox.gameNumber("SummonDuration", 2) or 150) + LOOK_SLACK
	end
	return longest :: number
end

-- The longest a charm at `level` takes (any creature's size), and a little more.
local function charmFrames(level: number): number
	local most = 0
	for _, name in { "CharmSmallDuration", "CharmMediumDuration", "CharmLargeDuration" } do
		most = math.max(most, nox.gameNumber(name, math.max(level, 1) - 1) or 0)
	end
	return (if most > 0 then most else 300) + LOOK_SLACK
end

local MONSTERS: any = { monsters = true }

-- A creature found: watched, on the party's team; a summoned one's health by its owner's tree.
local function found(id: number, c: any, o: any)
	known[id] = c.caster
	nox.watchEvents(id, nox.events.damage + nox.events.death)
	team.join(id)
	if c.charm then
		monsters.forget(id)
		return
	end
	local h = hero.byUnit(c.caster)
	local pct: number = if h then h.bonus.summonHealth or 0 else 0
	if pct ~= 0 and o.maxHealth > 0 then
		local most = math.clamp(math.floor(o.maxHealth * (1 + pct / 100) + 0.5), 1, 65535)
		nox.setHealth(id, math.clamp(math.floor(o.health * most / o.maxHealth + 0.5), 1, most), most)
		nox.touch(id)
	end
end

local function look()
	local now = nox.frame()
	for i = #coming, 1, -1 do
		local c = coming[i]
		local hit = false
		local x, y, reach = c.x, c.y, LOOK_REACH
		if c.charm and nox.present(c.caster) then
			x, y = nox.position(c.caster)
			reach = CHARM_REACH
		end
		for _, id in nox.unitsNear(x, y, reach, MONSTERS) do
			if not known[id] then
				local o = nox.object(id)
				if o and o.owner == c.caster and o.alive and (if c.charm then monsters.info(id) ~= nil else o.type == c.type) then
					found(id, c, o)
					hit = true
					break
				end
			end
		end
		if hit or now >= c.ends or not nox.present(c.caster) then
			table.remove(coming, i)
		end
	end
	if #coming == 0 and beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

-- A spell cast (main.luau's onSpellCast): a hero's summoning watched for its creature, a hero's
-- charm for the creature it makes its own.
function summons.cast(caster: number, spell: string | number)
	if type(spell) ~= "string" then
		return
	end
	local kind = typeOf[spell]
	local charm = spell == "SPELL_CHARM"
	if (not kind and not charm) or not hero.byUnit(caster) or not nox.present(caster) then
		return
	end
	local x, y = nox.position(caster)
	local wait = if charm then charmFrames(nox.spellLevel(caster, spell)) else waitFrames()
	table.insert(coming, { caster = caster, x = x, y = y, type = kind, charm = charm, ends = nox.frame() + wait })
	if not beat then
		beat = nox.every(LOOK_EVERY, look)
	end
end

-- The unit of the hero whose creature `id` is (summoned or charmed; nil: none of the party's).
function summons.ownerOf(id: number): number?
	local owner = known[id]
	if not owner then
		return nil
	end
	local o = nox.object(id)
	if o and o.owner == owner then
		return owner
	end
	known[id] = nil
	return nil
end

-- A watched creature died.
function summons.killed(id: number)
	known[id] = nil
end

-- A new map: the old map's creatures and summonings are gone with it.
function summons.reset()
	table.clear(known)
	table.clear(coming)
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

-- The known creatures as lines, "id type owner" (for "dd summons"), by id.
function summons.lines(): { string }
	local ids: { number } = {}
	for id in known do
		table.insert(ids, id)
	end
	table.sort(ids)
	local out = {}
	for _, id in ids do
		local o = nox.object(id)
		local h = hero.byUnit(known[id])
		table.insert(out, string.format("%d %s %s", id, if o then o.type else "-", if h then tostring(h.record.identity.name) else "-"))
	end
	return out
end

return summons
