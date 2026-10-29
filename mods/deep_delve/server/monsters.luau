--!strict
-- Monsters (design 3.3): packs made at the generator's spots, each monster's tier set (health,
-- enchantments, a name), and what the rest of the mod asks of them: a death's experience and loot,
-- the damage a monster deals.
--
-- What the tiers change today: health (nox.setHealth), the damage the monster deals (combat.luau
-- scales it through onDamage), enchantments (haste stands in for speed) and a rare's or champion's
-- name over its head now and then (nox.unitSay). Engine ask A7 (a coloured name plate, speed and
-- damage multipliers) replaces the stand-ins in `present` below, and nothing else.

--
-- Party scaling (scale.luau, design 7): a monster's most health is its own times the party's
-- health factor, changed on every live one as players come and go (rescale); the damage it deals
-- and the experience it gives are multiplied by the party's factors when they're used; an area
-- entered with more players gets extra packs at the generator's spots.

local data = require("data/monsters")
local numbers = require("data/numbers")
local scale = require("scale")

local monsters = {}

-- Each live monster the mod made: id -> { type, tier, name?, level, damage, experience, base (its
-- most health for one player) }.
local known: { [number]: any } = {}
-- The named ones (their names shown again now and then): a list of ids.
local named: { number } = {}
local nameTimer: number? = nil
local packsMade = 0
-- The packs the last area entered was given, in order (for "dd packs"): { along, size (its
-- monsters, leader too), tier (its leader's), leader (its type), experience (its monsters' all
-- told, for one player), rising (how many of them rise again after death) }.
local packList: { any } = {}
-- The end chamber's champion and its guards in the last area entered: { size, experience }, or nil.
local guarded: any = nil
local NEAR_PLAYERS: any = { players = true, alive = true }

function monsters.info(id: number): any
	return known[id]
end

-- How many the mod made are alive now (for "dd status").
function monsters.count(): number
	local n = 0
	for _ in known do
		n += 1
	end
	return n
end

-- ---- Presentation: A7's stand-ins, in one place

local present = {}

-- A name over a monster in its tier's colour. (A7: a name plate. Today: a speech bubble, shown
-- again by the name timer; the colour waits for A7.)
function present.name(id: number, name: string, _colour: string)
	nox.unitSay(id, name, 90)
end

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
-- entries that count from that), by weight; `noRising`: none that rises again after its death (a
-- roster entry's `rises`); `lightOnly`: only light ones (`light`), when the roster has any there.
local function pickType(roster: any, area: any, noRising: boolean?, lightOnly: boolean?): string?
	local list: { any }, total = {}, 0
	local function gather(light: boolean)
		for _, e in roster do
			if ((e.at and e.at <= area.index) or (e.from and e.from <= area.level)) and not (noRising and e.rises) and (not light or e.light) then
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

-- Sets a made monster up as `tier` at the area level: health, enchantments, its name; registered
-- and watched (its death, the mod's loot and experience).
function monsters.applyTier(id: number, thingType: string, tierName: string, level: number, fixed: any): any
	local tier = (data.tiers :: any)[tierName]
	local a = numbers.areas
	local base = nox.monsterInfo(thingType)
	local o = nox.object(id)
	local most = if o then o.maxHealth else (if base then base.health else 10)
	-- (the opening's areas: their monsters weaker, numbers.areas.opening)
	local opening: any = (a.opening :: any)[level]
	local openHealth = if opening then opening.health else 1
	local openDamage = if opening then opening.damage else 1
	local health = math.clamp(math.floor(most * (1 + a.healthPerLevel * (level - 1)) * tier.health * openHealth + 0.5), 1, 65535)
	local alone = health
	local party = scale.factor("health")
	if party ~= 1 then
		health = math.clamp(math.floor(alone * party + 0.5), 1, 65535)
	end
	nox.setHealth(id, health, health)
	nox.touch(id)
	local entry: any = {
		type = thingType,
		tier = tierName,
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
		table.insert(named, id)
	end
	known[id] = entry
	-- Its death (experience, loot) and the hits it takes (combat.luau's affixes).
	nox.watchEvents(id, nox.events.death + nox.events.damage)
	return entry
end

-- One monster of a type at (x, y), set up as `tier` (`fixed`: a champion's or boss's own set);
-- its id.
local function spawn(thingType: string, x: number, y: number, tierName: string, level: number, fixed: any?): number?
	local px, py = nox.clearPointNear(x, y, 40)
	local id = nox.create(thingType, px, py)
	if not id then
		nox.log("deep_delve: no monster type " .. thingType)
		return nil
	end
	monsters.applyTier(id, thingType, tierName, level, fixed)
	return id
end
monsters.spawn = spawn

-- An area's monsters at the generator's spots: a pack at each (a leader of the pack's tier and
-- its followers), the champion and its guards in the end chamber (a lair has no champion's spot:
-- its boss is boss.luau's). `area`: areas.info's ({ biome, level, index }).
function monsters.populate(spots: any, area: any)
	local roster = (data.rosters :: any)[area.biome] or data.rosters.Crypt
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
	local champ = (data.champions :: any)[area.biome]
	guarded = nil
	if champ and spots.guardian then
		local g = spots.guardian
		local group = { size = 0, experience = 0 }
		local function made(id: number?)
			local e = id and known[id]
			if e then
				group.size += 1
				group.experience += e.experience
			end
		end
		made(spawn(champ.type, g.x, g.y, "champion", area.level, champ))
		for _ = 1, if opening and opening.guards then opening.guards else champ.guards do
			made(spawn(pickType(roster, area) or champ.type, g.x, g.y, "magic", area.level))
		end
		guarded = group
	end
	-- The named ones' names again now and then while a player is near (one timer for all of
	-- them; a client shows a far one's words at the edge of its screen).
	if not nameTimer then
		nameTimer = nox.every(numbers.timing.nameEvery, function()
			local n = nox.keepLiving(named)
			for i = 1, n do
				local id = named[i]
				local e = known[id]
				if e and e.name then
					local x, y = nox.position(id)
					if nox.countNear(x, y, numbers.timing.nameReach, NEAR_PLAYERS) > 0 then
						present.name(id, e.name, (data.tiers :: any)[e.tier].colour)
					end
				end
			end
		end)
	end
end

-- A watched monster's death: its entry (forgotten here), or nil when it wasn't one of the mod's.
function monsters.killed(id: number): any
	local e = known[id]
	known[id] = nil
	return e
end

-- A monster that's no longer the mod's (a hero charmed it: summons.luau): forgotten, so its death
-- gives no experience or loot and the party's size no longer scales it; its entry, or nil.
function monsters.forget(id: number): any
	return monsters.killed(id)
end

-- The damage multiplier of a monster dealing a hit (1 for anything the mod didn't make), the
-- party's factor in it.
function monsters.damageOf(id: number): number
	local e = known[id]
	return if e then e.damage * scale.factor("damage") else 1
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
		if o and o.alive and o.maxHealth > 0 then
			local most = math.clamp(math.floor(e.base * f + 0.5), 1, 65535)
			if most ~= o.maxHealth then
				local health = math.clamp(math.floor(o.health * most / o.maxHealth + 0.5), 1, most)
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

-- The champion's group in the last area entered ({ size, experience }), or nil.
function monsters.guardians(): any
	return guarded
end

-- A new map: the old monsters are gone with it.
function monsters.reset()
	table.clear(known)
	table.clear(named)
end

return monsters
