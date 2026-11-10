--!strict
-- Monsters (design 3.3): packs made at the generator's spots, each monster's tier set (health,
-- enchantments, a name), and what the rest of the mod asks of them: a death's experience and loot,
-- the damage a monster deals.
--
-- What the tiers change today: health (nox.setHealth), the damage the monster deals (combat.luau
-- scales it through onDamage), enchantments (haste stands in for speed) and a name for a rare, a
-- champion or the act boss, drawn over its head in its tier's colour on NoX Core clients (a world
-- label: labels.luau sends each player's UI the named ones, from the index `named` here). Engine
-- ask A7's speed and damage multipliers would replace the stand-in in `present` below.

--
-- Party scaling (scale.luau, design 7): a monster's most health is its own times the party's
-- health factor, changed on every live one as players come and go (rescale); the damage it deals
-- and the experience it gives are multiplied by the party's factors when they're used; an area
-- entered with more players gets extra packs at the generator's spots.

--
-- Powers (powers.luau): a rare rolls one, a champion two, as it's set up (applyTier); a powered
-- monster's death tells powers.luau (its splitting, its lightning, its minions gone), and a new map
-- stops their beats. Each power adds to its experience (data's powerExperience). A summoner's
-- minions are the mod's monsters too (the party rule, the scaling), their entries marked `minion`
-- (and `master`, the summoner's id): main.luau gives no experience or loot for their deaths.

local data = require("data/monsters")
local numbers = require("data/numbers")
local powers = require("powers")
local scale = require("scale")

local monsters = {}

-- A roster type and its monster.bin health (weakTypes).
type Kind = { type: string, health: number }

-- Each live monster the mod made: id -> { type, tier, name?, code (its net code), level, damage,
-- experience, base (its most health for one player), died (one that rises again: it has died
-- once, its death's experience and loot given) }. One that rises again stays here while it lies
-- dead (monsters.killed).
local known: { [number]: any } = {}
-- The live ones with a name (their plates): id -> its entry in `known`.
local named: { [number]: any } = {}
-- The named or powered ones that rise again lying dead now, their plates down and their powers'
-- beats stopped (monsters.killed): id -> its entry in `known`.
local lying: { [number]: any } = {}
-- Called as a named monster comes or goes (labels.luau's plates), or nil.
local namedChanged: (() -> ())? = nil
local packsMade = 0
-- The packs the last area entered was given, in order (for "dd packs"): { along, size (its
-- monsters, leader too), tier (its leader's), leader (its type), experience (its monsters' all
-- told, for one player), rising (how many of them rise again after death) }.
local packList: { any } = {}
-- The end chamber's champion and its guards in the last area entered: { size, experience }, or nil.
local guarded: any = nil
-- The area the map's monsters were put in (populate's): { roster, area }, or nil before.
local here: any = nil
-- The roster's weakest types there (weakTypes), worked out once a map.
local weakest: { string }? = nil
-- How far the monsters made in this map look for the party (its area's biome's: data/acts.luau's
-- sights), or nil: the engine's own.
local sight: number? = nil

-- The map's monsters' look (nil: the engine's own), for every one made from now.
function monsters.setSight(range: number?)
	sight = range
end

-- What it is now (for "dd sight").
function monsters.sight(): number?
	return sight
end

function monsters.info(id: number): any
	return known[id]
end

-- Every live monster the mod made (id -> entry; a zombie lying dead too: its `died`); the index
-- itself, read only.
function monsters.all(): { [number]: any }
	return known
end

-- The live named monsters (id -> entry: its name, tier and net code); the index itself, read only.
function monsters.named(): { [number]: any }
	return named
end

-- `fn` called as a named monster comes or goes (made, killed, forgotten).
function monsters.onNamedChanged(fn: () -> ())
	namedChanged = fn
end

local function namedNow()
	local fn = namedChanged
	if fn then
		fn()
	end
end

-- How many the mod made are alive now (for "dd status"; one that rises again only while it
-- stands).
function monsters.count(): number
	local n = 0
	for id, e in known do
		if not e.died or nox.living(id) then
			n += 1
		end
	end
	return n
end

-- ---- Presentation: A7's stand-in

local present = {}

-- A monster's speed multiplier. (A7: the multiplier itself. Today: faster is haste.)
function present.speed(id: number, multiplier: number)
	if multiplier > 1 then
		nox.enchant(id, "ENCHANT_HASTED", 0)
	end
end

monsters.present = present

-- ---- Making them

-- The types that rise again after their death (a roster entry's `rises`), as a set, built once.
local risingTypes: { [string]: boolean } = {}
for _, roster in (data.rosters :: any) do
	for _, e in roster do
		if e.rises then
			risingTypes[e.type] = true
		end
	end
end

-- Whether a type rises again after its death (the rosters' `rises`).
function monsters.rises(thingType: string): boolean
	return risingTypes[thingType] == true
end

-- A type of the roster for an area (areas.info's: its level, and its index in its act for the
-- entries that count from that; an expedition's `rosterAt` in place of the index), by weight;
-- `noRising`: none that rises again after its death (a roster entry's `rises`); `lightOnly`: only
-- light ones (`light`), when the roster has any there.
local function pickType(roster: any, area: any, noRising: boolean?, lightOnly: boolean?): string?
	local list: { any }, total = {}, 0
	local index: number = area.rosterAt or area.index
	local function gather(light: boolean)
		for _, e in roster do
			if ((e.at and e.at <= index) or (e.from and e.from <= area.level)) and not (noRising and e.rises) and (not light or e.light) then
				table.insert(list, e)
				total += e.weight
			end
		end
	end
	gather(lightOnly == true)
	if #list == 0 and lightOnly then
		gather(false)
	end
	if #list == 0 then
		return nil
	end
	local roll = nox.random() * total
	for _, e in list do
		roll -= e.weight
		if roll < 0 then
			return e.type
		end
	end
	return list[#list].type
end

local function rareName(): string
	local n = data.rareNames
	return n.first[nox.random(#n.first)] .. n.second[nox.random(#n.second)]
end

-- A pack leader's tier at the area level.
local function rollTier(level: number): string
	local t = data.packTiers
	local rare = t.rare + t.perLevel.rare * level
	local magic = t.magic + t.perLevel.magic * level
	local roll = nox.random() * 100
	if roll < rare then
		return "rare"
	elseif roll < rare + magic then
		return "magic"
	end
	return "normal"
end

-- Sets a made monster up as `tier` at the area level: health, enchantments, its name, its powers
-- (`want`: these powers' ids, {} none; nil: its tier's count rolled); registered and watched (its
-- death, the mod's loot and experience).
function monsters.applyTier(id: number, thingType: string, tierName: string, level: number, fixed: any, want: { string }?): any
	local tier = (data.tiers :: any)[tierName]
	local a = numbers.areas
	local base = nox.monsterInfo(thingType)
	local o = nox.object(id)
	local most = if o then o.maxHealth else (if base then base.health else 10)
	-- (the opening's areas: their monsters weaker, numbers.areas.opening)
	local opening: any = (a.opening :: any)[level]
	local openHealth = if opening then opening.health else 1
	local openDamage = if opening then opening.damage else 1
	-- (a boss's own factor on its tier's: data/monsters.luau's bosses' `health`; else its type's,
	-- data/monsters.luau's typeHealth)
	local own: number = if fixed and type(fixed.health) == "number" then fixed.health else ((data.typeHealth :: any)[thingType] or 1)
	local health = math.clamp(math.floor(most * (1 + a.healthPerLevel * (level - 1)) * tier.health * openHealth * own + 0.5), 1, 65535)
	local alone = health
	local party = scale.factor("health")
	if party ~= 1 then
		health = math.clamp(math.floor(alone * party + 0.5), 1, 65535)
	end
	nox.setHealth(id, health, health)
	nox.touch(id)
	-- Its look: the map's, when it has one and the monster moves (one that never moves keeps its
	-- own reach). Set once, as it's made.
	local reach = sight
	if reach and not (data.powerKinds.still :: any)[thingType] then
		-- (sightRange refuses what isn't a creature)
		local ok, now = pcall(function(): number
			return nox.sightRange(id)
		end)
		if ok and now < reach then
			nox.sightRange(id, reach)
		end
	end
	local entry: any = {
		type = thingType,
		tier = tierName,
		code = if o then o.netCode else 0,
		level = level,
		base = alone,
		damage = tier.damage * (1 + a.damagePerLevel * (level - 1)) * openDamage,
		-- (monster.bin's experience is the stock game's, thousands a kill: scaled to Deep Delve's
		-- own curve, at least 1)
		experience = math.max(1, math.floor((if base then base.experience * a.experienceScale else 10) * tier.experience
			* (1 + a.experiencePerLevel * (level - 1)) + 0.5)),
		drops = tier.drops,
	}
	-- Enchantments: a champion's own set, else as many of the tier's list as it gets.
	if fixed then
		for _, e in fixed.enchantments do
			if e == "ENCHANT_HASTED" then
				present.speed(id, 1.5)
			else
				nox.enchant(id, e, 0)
			end
		end
	else
		local pool = table.clone(data.enchantments)
		for _ = 1, math.min(tier.enchantments, #pool) do
			local e: string = table.remove(pool, nox.random(#pool)) :: string
			if e == "ENCHANT_HASTED" then
				present.speed(id, 1.5)
			else
				nox.enchant(id, e, 0)
			end
		end
	end
	if tier.named then
		entry.name = if fixed then fixed.name else rareName()
	end
	-- Its powers (a rare's one, a champion's two), their words on its plate.
	if want or tier.powers then
		powers.give(id, entry, tier.powers or 0, want)
	end
	known[id] = entry
	if entry.name then
		named[id] = entry
		namedNow()
	end
	-- Its death (experience, loot) and the hits it takes (combat.luau's affixes); one that rises
	-- again, its body's end too (monsters.gone).
	local watch = nox.events.death + nox.events.damage
	if risingTypes[thingType] then
		watch += nox.events.destroyed
	end
	nox.watchEvents(id, watch)
	return entry
end

-- One monster of a type at (x, y), set up as `tier` (`fixed`: a champion's or boss's own set;
-- `want`: its powers, applyTier's); its id.
local function spawn(thingType: string, x: number, y: number, tierName: string, level: number, fixed: any?, want: { string }?): number?
	local px, py = nox.clearPointNear(x, y, 40)
	local id = nox.create(thingType, px, py)
	if not id then
		nox.log("deep_delve: no monster type " .. thingType)
		return nil
	end
	monsters.applyTier(id, thingType, tierName, level, fixed, want)
	return id
end
monsters.spawn = spawn

-- An area's monsters at the generator's spots: a pack at each (a leader of the pack's tier and
-- its followers), the champion and its guards in the end chamber (a lair has no champion's spot:
-- its boss is boss.luau's). `area`: areas.info's ({ roster (its biome's, or the area's own), level,
-- index }; an expedition's map hands its roster and champion themselves, runRoster and
-- runChampion, and its guards).
function monsters.populate(spots: any, area: any)
	local rosterName = area.roster or area.biome
	local roster = area.runRoster or (data.rosters :: any)[rosterName] or data.rosters.Crypt
	here = { roster = roster, area = area }
	weakest = nil
	-- The opening's numbers at this level (numbers.areas.opening): smaller packs, the first stretch
	-- of the route quieter still.
	local opening: any = if area.lair then nil else (numbers.areas.opening :: any)[area.level]
	local f = numbers.areas.followers
	if opening then
		f = { fewest = opening.followers[1], most = opening.followers[2] }
	end
	-- Where nothing that rises again may stand (data/monsters.luau's rising): the rooms players
	-- arrive in (the start, a waypoint's arrival), and the route's first stretch in the first areas.
	local rising = data.rising
	local clearUntil: number = (rising.clearUntil :: any)[area.level] or 0
	local arrivals = { spots.arrive, spots.wayArrive }
	local function noRising(spot: any): boolean
		if (tonumber(spot.along) or 0) < clearUntil then
			return true
		end
		for _, p in arrivals do
			if p and math.abs(p.x - spot.x) + math.abs(p.y - spot.y) <= rising.arrivalReach then
				return true
			end
		end
		return false
	end
	-- (a party: extra packs at spots drawn from the generator's, beside the pack there)
	local at: { any } = table.clone(spots.packs or {})
	local extra = math.floor(#at * (scale.factor("packs") - 1) + 0.5)
	local count = #at
	if count > 0 then
		for _ = 1, extra do
			table.insert(at, at[nox.random(count)])
		end
	end
	packsMade = #at
	table.clear(packList)
	for _, spot in at do
		local plain = noRising(spot)
		-- (the opening's first stretch: light monsters only, numbers.areas.opening's `light`)
		local light = opening ~= nil and (tonumber(spot.along) or 0) < (opening.light or 0)
		local leaderType = pickType(roster, area, plain, light)
		if leaderType then
			local along = tonumber(spot.along) or 0
			-- (the opening's quiet stretch: a normal leader and its fewest followers)
			local quiet = opening ~= nil and along < opening.quiet
			local tier = if quiet then "normal" else rollTier(area.level)
			local followers = if quiet then f.fewest else nox.random(f.fewest, f.most)
			local pack = { along = along, size = 0, tier = tier, leader = leaderType, experience = 0, rising = 0 }
			local function made(id: number?)
				local e = id and known[id]
				if e then
					pack.size += 1
					pack.experience += e.experience
					if risingTypes[e.type] then
						pack.rising += 1
					end
				end
			end
			made(spawn(leaderType, spot.x, spot.y, tier, area.level))
			-- Followers: the leader's kind or another of the roster's; a rare leader's are magic.
			for _ = 1, followers do
				local t = if nox.random() < 0.5 then leaderType else (pickType(roster, area, plain, light) or leaderType)
				made(spawn(t, spot.x, spot.y, if tier == "rare" then "magic" else "normal", area.level))
			end
			table.insert(packList, pack)
		end
	end
	local champ = area.runChampion or (data.champions :: any)[rosterName]
	guarded = nil
	if champ and spots.guardian then
		local g = spots.guardian
		local group: any = { size = 0, experience = 0 }
		local function made(id: number?)
			local e = id and known[id]
			if e then
				group.size += 1
				group.experience += e.experience
			end
		end
		-- (the champion's id kept: an expedition's run ends with its death)
		group.id = spawn(champ.type, g.x, g.y, "champion", area.level, champ)
		made(group.id)
		for _ = 1, if opening and opening.guards then opening.guards else (area.guards or champ.guards) do
			made(spawn(pickType(roster, area) or champ.type, g.x, g.y, "magic", area.level))
		end
		guarded = group
	end
end

-- The `n` weakest types of the roster where the monsters are now (by monster.bin's health, the
-- lightest first; those the area has at its level or index, as packs draw them; none that rises
-- again, none big or still: data's powerKinds), for a summoner's minions (powers.luau). Before any
-- area (a monster made in town): the Crypt's at the first level. Worked out once a map.
function monsters.weakTypes(n: number): { string }
	local cached = weakest
	if cached then
		return cached
	end
	local roster: any = if here then here.roster else data.rosters.Crypt
	local area: any = if here then here.area else { index = 1, level = 1 }
	local index: number = area.rosterAt or area.index or 1
	local kinds: any = data.powerKinds
	local list: { Kind } = {}
	local seen: { [string]: boolean } = {}
	for _, e in roster do
		local t: string = e.type
		if ((e.at and e.at <= index) or (e.from and e.from <= area.level)) and not e.rises and not seen[t] and not kinds.big[t]
			and not kinds.still[t] then
			seen[t] = true
			local info = nox.monsterInfo(t)
			table.insert(list, { type = t, health = if info then info.health else math.huge })
		end
	end
	table.sort(list, function(a: Kind, b: Kind): boolean
		if a.health ~= b.health then
			return a.health < b.health
		end
		return a.type < b.type
	end)
	local out: { string } = {}
	for i = 1, math.min(n, #list) do
		table.insert(out, list[i].type)
	end
	weakest = out
	return out
end

-- A monster out of the index (and its plate's): its entry, or nil.
local function drop(id: number): any
	local e = known[id]
	known[id] = nil
	lying[id] = nil
	if named[id] then
		named[id] = nil
		namedNow()
	end
	return e
end

-- A watched monster's death: its entry, or nil when it wasn't one of the mod's or it has died
-- before. A powered one's death powers go off (powers.luau), at its first death.
--
-- One that rises again (a zombie: the engine stands it up again, whole, a while after it fell,
-- unless it burned) stays one of the mod's while it lies dead and after it rises (by decision
-- 2026-11-10): its hits on the heroes scaled, the heroes' on it worked as on any (the warrior's
-- skills, its Rage). Its plate comes down while it lies (monsters.standing puts it back). Only its
-- first death gives anything (by decision 2026-10-29: a risen zombie gives nothing), and it's
-- forgotten only as its body goes (monsters.gone) or with the map.
--
-- Its powers that act by themselves (their beat: burning, a summoner's minions and the rest) stop
-- at each of its deaths, its minions crumbling, and start again as it was made when it stands
-- (by decision 2026-11-10; monsters.standing). Its death powers (splitting, stormcharged) go off at
-- its first death alone, with the rest of what that death gives.
function monsters.killed(id: number): any
	local e = known[id]
	if e and risingTypes[e.type] then
		if e.name or e.powers then
			lying[id] = e
		end
		if named[id] then
			named[id] = nil
			namedNow()
		end
		local first = not e.died
		e.died = true
		if e.powers then
			powers.died(id, e, not first)
		end
		return if first then e else nil
	end
	e = drop(id)
	if e and e.powers then
		powers.died(id, e)
	end
	return e
end

-- A watched thing gone from the world (main.luau's onDestroyed: a zombie burnt away, a body
-- deleted): one of the mod's that rises again forgotten now. `thingType`: the gone thing's, so
-- that an id made again since isn't taken for it.
function monsters.gone(id: number, thingType: string?)
	local e = known[id]
	if not e or (thingType ~= nil and e.type ~= thingType) or nox.present(id) then
		return
	end
	drop(id)
	if e.powers then
		powers.forgot(id, e)
	end
end

-- The ones lying dead that the engine has stood up again: a named one's plate back, a powered
-- one's beat started again as it was made (powers.rose) (labels.luau's beat; a walk of those lying
-- alone).
function monsters.standing()
	local any = false
	for id, e in lying do
		if nox.living(id) then
			lying[id] = nil
			if e.name then
				named[id] = e
				any = true
			end
			if e.powers then
				powers.rose(id, e)
			end
		elseif not nox.present(id) then
			lying[id] = nil
		end
	end
	if any then
		namedNow()
	end
end

-- A monster that's no longer the mod's (a hero charmed it: summons.luau): forgotten, so its death
-- gives no experience or loot and the party's size no longer scales it, and its powers' beat
-- stops; its entry, or nil.
function monsters.forget(id: number): any
	local e = drop(id)
	if e and e.powers then
		powers.forgot(id, e)
	end
	return e
end

-- The damage multiplier of a monster dealing a hit (1 for anything the mod didn't make), the
-- party's factor in it.
function monsters.damageOf(id: number): number
	local e = known[id]
	return if e then e.damage * scale.factor("damage") else 1
end

-- The share of its damage a monster's spells deal (what it owns: a boss's mist, its fist): its
-- entry's `spells` (a boss's, data/monsters.luau), else 1.
function monsters.spellsOf(id: number): number
	local e = known[id]
	return if e and type(e.spells) == "number" then e.spells else 1
end

-- The experience a monster's death gives (before it's split), the party's factor in it.
function monsters.experienceOf(e: any): number
	local f = scale.factor("experience")
	if f == 1 then
		return e.experience
	end
	return math.max(1, math.floor(e.experience * f + 0.5))
end

-- Every live monster's most health for the party now (its share of health kept), as players come
-- and go. A walk of the mod's own monsters, only when the count changes.
function monsters.rescale()
	local f = scale.factor("health")
	for id, e in known do
		local o = nox.object(id)
		if o and o.maxHealth > 0 and (o.alive or e.died) then
			local most = math.clamp(math.floor(e.base * f + 0.5), 1, 65535)
			if most ~= o.maxHealth then
				-- (one that rises again lying dead: only its most, which it rises with)
				local health = if o.alive then math.clamp(math.floor(o.health * most / o.maxHealth + 0.5), 1, most) else 0
				nox.setHealth(id, health, most)
				nox.touch(id)
			end
		end
	end
end

-- The mod's monster with the lowest id and its numbers, for "dd scale": { id, health, most, base,
-- damage (with the party's factor), experience (likewise) }, or nil.
function monsters.sample(): any
	local best = nil
	for id in known do
		if (not best or id < best) and nox.living(id) then
			best = id
		end
	end
	if not best then
		return nil
	end
	local o = nox.object(best)
	local e = known[best]
	return {
		id = best,
		health = if o then o.health else 0,
		most = if o then o.maxHealth else 0,
		base = e.base,
		damage = monsters.damageOf(best),
		experience = monsters.experienceOf(e),
	}
end

-- How many packs the last area entered was given (its spots' and a party's extra).
function monsters.packsMade(): number
	return packsMade
end

-- The packs the last area entered was given, in order (each { along, size, tier, leader,
-- experience }).
function monsters.packs(): { any }
	return packList
end

-- The champion's group in the last area entered ({ size, experience, id (the champion's) }), or
-- nil.
function monsters.guardians(): any
	return guarded
end

-- A new map: the old monsters are gone with it (labels.luau's reset lets their plates go).
function monsters.reset()
	table.clear(known)
	table.clear(named)
	table.clear(lying)
	here = nil
	weakest = nil
	sight = nil
	powers.reset()
end

powers.attach(monsters)

return monsters
