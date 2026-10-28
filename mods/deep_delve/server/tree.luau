--!strict
-- The passive tree's rules (design 4.5; its shape is data/tree.luau). Everything here reads or
-- writes one record; the hero's numbers, book and save follow in hero.luau (hero.treeChanged).
--
-- The record's part (store.luau's version 3): tree = { nodes = { ids taken }, bossPoints,
-- respecs }. A class's root is always taken, for nothing.
--
-- Points: one for each level the character has reached, and one the first time it clears each
-- act at each difficulty tier (an act boss's kill). A node may be taken with a point to spare
-- when it is joined to one taken already.

local data = require("data/tree")

local tree = {}

local nodes = data.nodes
-- Each node's neighbours (both ways), built once.
local near: { [number]: { number } } = {}
for _, n in nodes do
	near[n.id] = near[n.id] or {}
	for _, other in n.links do
		near[other] = near[other] or {}
		table.insert(near[n.id], other)
		table.insert(near[other], n.id)
	end
end
-- The abilities by number, as an index.
local abilityById: { [number]: any } = {}
for _, a in data.abilities do
	abilityById[a.id] = a
end

-- The tree's spells' numbers by name (nox.spellId), worked out once: its spells, its creatures'
-- summon spells and the guide's first summon.
local spellIds: { [string]: number }? = nil
local function idIndex(): { [string]: number }
	if spellIds then
		return spellIds
	end
	local ids: { [string]: number } = {}
	local function put(name: string)
		local id = nox.spellId(name)
		if id then
			ids[name] = id
		end
	end
	for name in (data.spellTitles :: any) do
		put(name)
	end
	for _, c in (data.creatures :: any) do
		put(c.spell)
	end
	put(data.firstSummon)
	spellIds = ids
	return ids
end

-- A spell's number by its name (one the tree names), nil when the game has no such spell.
function tree.spellId(name: string): number?
	return idIndex()[name]
end

-- The tree's creatures' places in the field guide (1-40) by name (nox.guideNumber), worked out
-- once.
local guideNumbers: { [string]: number }? = nil

-- A creature's place in the field guide (1-40), the engine's for its type's name; nil when it has
-- none or the tree names no such creature.
function tree.guideNumber(creature: string): number?
	if not guideNumbers then
		local n: { [string]: number } = {}
		for name in (data.creatures :: any) do
			local g = nox.guideNumber(name)
			if g then
				n[name] = g
			end
		end
		guideNumbers = n
	end
	return (guideNumbers :: { [string]: number })[creature]
end

-- A creature's summon spell's number, nil when unknown.
function tree.summonSpell(creature: string): number?
	local c = (data.creatures :: any)[creature]
	return c and tree.spellId(c.spell)
end

function tree.node(id: number): any
	return nodes[id]
end

function tree.count(): number
	return #nodes
end

function tree.neighbours(id: number): { number }
	return near[id] or {}
end

function tree.ability(id: number): any
	return abilityById[id]
end

-- The record's part, made whole (an old or new record).
function tree.of(rec: any): any
	local t = rec.tree
	if type(t) ~= "table" then
		t = {}
		rec.tree = t
	end
	if type(t.nodes) ~= "table" then
		t.nodes = {}
	end
	t.bossPoints = if type(t.bossPoints) == "number" then t.bossPoints else 0
	t.respecs = if type(t.respecs) == "number" then t.respecs else 0
	return t
end

local function classOf(rec: any): string
	return rec.identity.class or "warrior"
end

-- The nodes taken, as a set (the class's root among them).
function tree.taken(rec: any): { [number]: boolean }
	local set: { [number]: boolean } = {}
	local root = data.roots[classOf(rec)]
	if root then
		set[root] = true
	end
	for _, id in tree.of(rec).nodes do
		set[id] = true
	end
	return set
end

-- Points: all it has earned, those spent, those left.
function tree.points(rec: any): (number, number, number)
	local t = tree.of(rec)
	local total = rec.level.level + t.bossPoints
	local spent = #t.nodes
	return total, spent, total - spent
end

-- Whether node `id` may be taken now, else why.
function tree.canTake(rec: any, id: number): (boolean, string?)
	local n = nodes[id]
	if not n then
		return false, "no such node"
	end
	local set = tree.taken(rec)
	if set[id] then
		return false, "taken already"
	end
	local _, _, left = tree.points(rec)
	if left < 1 then
		return false, "no points left"
	end
	for _, other in near[id] or {} do
		if set[other] then
			return true, nil
		end
	end
	return false, "not joined to a node you have"
end

-- Node `id` taken (the record only); whether it was, else why.
function tree.take(rec: any, id: number): (boolean, string?)
	local ok, why = tree.canTake(rec, id)
	if not ok then
		return false, why
	end
	table.insert(tree.of(rec).nodes, id)
	return true, nil
end

-- What a respec costs now, in gold.
function tree.respecCost(rec: any): number
	return data.respec.base + data.respec.perNode * #tree.of(rec).nodes
end

-- Every node given back (the record only; the gold is the caller's).
function tree.respec(rec: any)
	local t = tree.of(rec)
	t.nodes = {}
	t.respecs += 1
end

-- An act cleared for the first time at the character's tier: a point.
function tree.bossPoint(rec: any)
	tree.of(rec).bossPoints += 1
end

-- How many nodes of each of the class's own spells and abilities are taken (by name; by number),
-- and the creatures its summon nodes give.
local function counted(rec: any): ({ [string]: number }, { [number]: number }, { [string]: boolean })
	local class = classOf(rec)
	local spells: { [string]: number } = {}
	local abilities: { [number]: number } = {}
	local creatures: { [string]: boolean } = {}
	for id in tree.taken(rec) do
		local n = nodes[id]
		if n and n.region == class then
			if n.kind == "spell" then
				spells[n.spell] = (spells[n.spell] or 0) + 1
			elseif n.kind == "ability" then
				abilities[n.ability] = (abilities[n.ability] or 0) + 1
			elseif n.kind == "summon" then
				creatures[n.creature] = true
			end
		end
	end
	return spells, abilities, creatures
end

-- A spell's or ability's level from its nodes taken: its start level, one more for each further
-- node, at most 5; and how many of those nodes give its mastery (each past its top level).
local function levelOf(give: string | number, count: number): (number, number)
	local r = data.rise(give)
	local raw = r.start + count - 1
	return math.min(5, raw), math.max(0, raw - r.top)
end

local function addStats(sum: any, stats: any, times: number)
	for k, v in stats do
		local was = sum[k]
		if was ~= nil then
			sum[k] = was + v * times
		end
	end
end

-- What the taken nodes add up to: { health, mana, strength, healthPct, manaPct, damage, resist,
-- own }: each node's stats, and the masteries of the class's own spells and abilities; own: the
-- % more damage a hit by a thing of each type deals (its spell's own bonus, data.rises), by type.
function tree.totals(rec: any): any
	local own: { [string]: number } = {}
	local sum = { health = 0, mana = 0, strength = 0, healthPct = 0, manaPct = 0, damage = 0, resist = 0, own = own }
	for id in tree.taken(rec) do
		local n = nodes[id]
		if n then
			addStats(sum, n.stats, 1)
		end
	end
	local spells, abilities = counted(rec)
	for name, count in spells do
		local r = data.rise(name)
		local _, past = levelOf(name, count)
		addStats(sum, r.mastery, past)
		if r.own ~= 0 then
			for _, kind in r.hits do
				own[kind] = (own[kind] or 0) + r.own
			end
		end
	end
	for a, count in abilities do
		local _, past = levelOf(a, count)
		addStats(sum, data.rise(a).mastery, past)
	end
	return sum
end

-- What the taken nodes give the character's class: { spells = { [name] = level },
-- abilities = { [id] = level }, creatures = { [creature] = true } }. A spell's level is its start
-- level (data.rises) and one more for each further node of it taken, at most 5.
function tree.wanted(rec: any): any
	local counts, abilityCounts, creatures = counted(rec)
	local spells: { [string]: number } = {}
	local abilities: { [number]: number } = {}
	for name, count in counts do
		spells[name] = levelOf(name, count)
	end
	for a, count in abilityCounts do
		abilities[a] = levelOf(a, count)
	end
	return { spells = spells, abilities = abilities, creatures = creatures }
end

-- A creature's name as the tree shows it.
function tree.creatureTitle(creature: string): string
	local c = (data.creatures :: any)[creature]
	return if c then c.title else creature
end

-- ---- Words

local STAT_WORDS: { { string } } = {
	{ "health", "+%d health" },
	{ "mana", "+%d mana" },
	{ "strength", "+%d strength" },
	{ "healthPct", "%+d%% most health" },
	{ "manaPct", "%+d%% most mana" },
	{ "damage", "%+d%% damage dealt" },
	{ "resist", "%+d%% resistance" },
}

-- What a set of stats says ("+8 health, +3% damage dealt").
function tree.statText(stats: any): string
	local parts = {}
	for _, w in STAT_WORDS do
		local v = stats[w[1]]
		if v and v ~= 0 then
			table.insert(parts, string.format(w[2], v))
		end
	end
	return table.concat(parts, ", ")
end

local CLASS_TITLES: { [string]: string } = { warrior = "Warrior", wizard = "Wizard", conjurer = "Conjurer" }

-- A node's name and what it does, as the tree window shows them.
function tree.describe(n: any): (string, string)
	if n.kind == "spell" or n.kind == "ability" then
		local title: string
		if n.kind == "spell" then
			title = (data.spellTitles :: any)[n.spell] or n.spell
		else
			local a = abilityById[n.ability]
			title = if a then a.title else "Ability " .. n.ability
		end
		local only = " (" .. CLASS_TITLES[n.region] .. " only)"
		local r = data.rise(n.spell or n.ability)
		local mastery = tree.statText(r.mastery)
		if n.first then
			local own = if r.own ~= 0 then string.format(", and %+d%% %s damage", r.own, title) else ""
			return title, "Unlocks " .. title .. " at level " .. n.level .. own .. only
		elseif n.beyond then
			return title .. " mastery", mastery .. only
		elseif n.mastery and mastery ~= "" then
			return title .. " " .. n.level, title .. " to level " .. n.level .. ", and " .. mastery .. only
		end
		return title .. " " .. n.level, title .. " to level " .. n.level .. only
	elseif n.kind == "summon" then
		local title = tree.creatureTitle(n.creature)
		return "Summon " .. title, "The " .. title .. " in your field guide, to summon (" .. CLASS_TITLES[n.region] .. " only)"
	elseif n.kind == "root" then
		return n.name, "Where the " .. CLASS_TITLES[n.region] .. " starts (free for a " .. CLASS_TITLES[n.region] .. ")"
	end
	local kind = if n.kind == "keystone" then "Keystone: " elseif n.kind == "notable" then "Notable: " else ""
	return n.name, kind .. tree.statText(n.stats)
end

return tree
