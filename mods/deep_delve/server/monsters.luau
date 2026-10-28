--!strict
-- Monsters (design 3.3): packs made at the generator's spots, each monster's tier set (health,
-- enchantments, a name), and what the rest of the mod asks of them: a death's experience and loot,
-- the damage a monster deals.
--
-- What the tiers change today: health (nox.setHealth), the damage the monster deals (combat.luau
-- scales it through onDamage), enchantments (haste stands in for speed) and a rare's or champion's
-- name over its head now and then (nox.unitSay). Engine ask A7 (a coloured name plate, speed and
-- damage multipliers) replaces the stand-ins in `present` below, and nothing else.

local data = require("data/monsters")
local numbers = require("data/numbers")

local monsters = {}

-- Each live monster the mod made: id -> { type, tier, name?, level, damage, experience }.
local known: { [number]: any } = {}
-- The named ones (their names shown again now and then): a list of ids.
local named: { number } = {}
local nameTimer: number? = nil
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

local function pickType(roster: any, level: number): string?
	local list: { any }, total = {}, 0
	for _, e in roster do
		if e.from <= level then
			table.insert(list, e)
			total += e.weight
		end
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
	local health = math.clamp(math.floor(most * (1 + a.healthPerLevel * (level - 1)) * tier.health + 0.5), 1, 65535)
	nox.setHealth(id, health, health)
	nox.touch(id)
	local entry: any = {
		type = thingType,
		tier = tierName,
		level = level,
		damage = tier.damage * (1 + a.damagePerLevel * (level - 1)),
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
-- its boss is boss.luau's). `area`: { biome, level }.
function monsters.populate(spots: any, area: any)
	local roster = (data.rosters :: any)[area.biome] or data.rosters.Crypt
	local f = numbers.areas.followers
	for _, spot in spots.packs or {} do
		local leaderType = pickType(roster, area.level)
		if leaderType then
			local tier = rollTier(area.level)
			spawn(leaderType, spot.x, spot.y, tier, area.level)
			-- Followers: the leader's kind or another of the roster's; a rare leader's are magic.
			for _ = 1, nox.random(f.fewest, f.most) do
				local t = if nox.random() < 0.5 then leaderType else (pickType(roster, area.level) or leaderType)
				spawn(t, spot.x, spot.y, if tier == "rare" then "magic" else "normal", area.level)
			end
		end
	end
	local champ = (data.champions :: any)[area.biome]
	if champ and spots.guardian then
		local g = spots.guardian
		spawn(champ.type, g.x, g.y, "champion", area.level, champ)
		for _ = 1, champ.guards do
			spawn(pickType(roster, area.level) or champ.type, g.x, g.y, "magic", area.level)
		end
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

-- The damage multiplier of a monster dealing a hit (1 for anything the mod didn't make).
function monsters.damageOf(id: number): number
	local e = known[id]
	return if e then e.damage else 1
end

-- A new map: the old monsters are gone with it.
function monsters.reset()
	table.clear(known)
	table.clear(named)
end

return monsters
