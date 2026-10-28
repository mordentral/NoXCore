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

-- What the taken nodes add up to: { health, mana, strength, healthPct, manaPct, damage, resist }.
function tree.totals(rec: any): any
	local sum = { health = 0, mana = 0, strength = 0, healthPct = 0, manaPct = 0, damage = 0, resist = 0 }
	for id in tree.taken(rec) do
		local n = nodes[id]
		if n then
			for k, v in n.stats do
				local was = (sum :: any)[k]
				if was ~= nil then
					(sum :: any)[k] = was + v
				end
			end
		end
	end
	return sum
end

-- What the taken nodes give the character's class: { spells = { [name] = level },
-- abilities = { [id] = level } }. A spell's level is the count of its nodes taken (at most 5).
function tree.wanted(rec: any): any
	local class = classOf(rec)
	local spells: { [string]: number } = {}
	local abilities: { [number]: number } = {}
	for id in tree.taken(rec) do
		local n = nodes[id]
		if n and n.region == class then
			if n.kind == "spell" then
				spells[n.spell] = math.min(5, (spells[n.spell] or 0) + 1)
			elseif n.kind == "ability" then
				abilities[n.ability] = math.min(5, (abilities[n.ability] or 0) + 1)
			end
		end
	end
	return { spells = spells, abilities = abilities }
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
	if n.kind == "spell" then
		local title = (data.spellTitles :: any)[n.spell] or n.spell
		local what = if n.level == 1 then "Unlocks " .. title .. " at level 1" else title .. " to level " .. n.level
		return title .. (if n.level > 1 then " " .. tostring(n.level) else ""), what .. " (" .. CLASS_TITLES[n.region] .. " only)"
	elseif n.kind == "ability" then
		local a = abilityById[n.ability]
		local title = if a then a.title else "Ability " .. n.ability
		local what = if n.level == 1 then "Unlocks " .. title .. " at level 1" else title .. " to level " .. n.level
		return title .. (if n.level > 1 then " " .. tostring(n.level) else ""), what .. " (Warrior only)"
	elseif n.kind == "root" then
		return n.name, "Where the " .. CLASS_TITLES[n.region] .. " starts (free for a " .. CLASS_TITLES[n.region] .. ")"
	end
	local kind = if n.kind == "keystone" then "Keystone: " elseif n.kind == "notable" then "Notable: " else ""
	return n.name, kind .. tree.statText(n.stats)
end

return tree
