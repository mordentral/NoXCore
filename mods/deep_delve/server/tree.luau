--!strict
-- The passive tree's rules (design 4.5; its shape is data/tree.luau). Everything here reads or
-- writes one record; the hero's numbers, book and save follow in hero.luau (hero.treeChanged).
--
-- The record's part (store.luau's version 3): tree = { nodes = { ids taken }, bossPoints,
-- respecs }. A class's root is always taken, for nothing.
--
-- Points: two at level 1 (its first spell and a first passive, by decision 2026-10-29), one more
-- for each level after it, and one the first time it clears each act at each difficulty tier (an
-- act boss's kill). Nothing of the count is kept in the record, so an older character has the
-- level-1 point too from its next load. A node may be taken with a point to spare
-- when it is joined to one taken already.
--
-- Another class's nodes (by decision 2026-10-29): a spell or summon node gives its spell or
-- creature to any class the game lets cast it (tree.usable), an ability node its ability to a
-- warrior; to any other taker its stats' worth (data.crossWorth, by the taker's class).
--
-- One node given back (by decision 2026-10-29): anywhere, for less gold than a respec
-- (data.refund), and only a node whose going leaves every other node taken joined to the root.

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

-- Whether a class may cast a spell (by name): the spell's class flags its own or any caster's,
-- as the game asks at the cast (a warrior casts none). Worked out once a class and spell.
local castCache: { [string]: { [string]: boolean } } = {}
local function castable(class: string, spell: string): boolean
	local bits = (data.classFlags :: any)[class]
	if not bits then
		return false
	end
	local known = castCache[class]
	if not known then
		known = {}
		castCache[class] = known
	end
	local can = known[spell]
	if can == nil then
		local info = nox.spellInfo(spell)
		can = info ~= nil and bit32.band(info.flags, bits + data.anyCaster) ~= 0
		known[spell] = can
	end
	return can
end

-- Whether a node's spell, ability or creature is a class's to have: a spell node's spell when the
-- class may cast it; a summon node's creature when the class may cast its summon spell (a
-- conjurer); an ability node's ability for a warrior. Its own region's always are; any other node
-- gives the same to every class (true).
function tree.usable(class: string, n: any): boolean
	if n.region == class then
		return true
	end
	if n.kind == "spell" then
		return castable(class, n.spell)
	elseif n.kind == "summon" then
		local c = (data.creatures :: any)[n.creature]
		return c ~= nil and castable(class, c.spell)
	elseif n.kind == "ability" then
		return class == "warrior"
	end
	return true
end

-- Whether a node gives something of its own (a spell, ability or creature) that a class may lack.
local function givesSkill(n: any): boolean
	return n.kind == "spell" or n.kind == "summon" or n.kind == "ability"
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

-- Points: all it has earned, those spent, those left (data.points: a level-1 character's, then one
-- a level, and the boss points).
function tree.points(rec: any): (number, number, number)
	local t = tree.of(rec)
	local total = data.points.atOne + rec.level.level - 1 + t.bossPoints
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

-- What giving one node back costs now, in gold: by the character's level, never more than a
-- whole respec.
function tree.refundCost(rec: any): number
	return math.min(data.refund.base + data.refund.perLevel * rec.level.level, tree.respecCost(rec))
end

-- Whether node `id` may be given back alone (the record only), else why: one taken (not the
-- class's root) whose going leaves every other node taken joined to the root by taken nodes.
function tree.canRefund(rec: any, id: number): (boolean, string?)
	local list = tree.of(rec).nodes
	local root = data.roots[classOf(rec)]
	if not table.find(list, id) then
		return false, if id == root then "the root stays" else "not taken"
	end
	local set: { [number]: boolean } = {}
	local others = 0
	for _, other in list do
		if other ~= id and not set[other] then
			set[other] = true
			others += 1
		end
	end
	-- (from the root through the nodes left, each counted once)
	local seen: { [number]: boolean } = {}
	local queue: { number } = {}
	if root then
		seen[root] = true
		table.insert(queue, root)
	end
	local reached = 0
	local head = 1
	while head <= #queue do
		local at = queue[head]
		head += 1
		for _, other in near[at] or {} do
			if set[other] and not seen[other] then
				seen[other] = true
				reached += 1
				table.insert(queue, other)
			end
		end
	end
	if reached < others then
		return false, "other nodes you have hang on it"
	end
	return true, nil
end

-- Node `id` given back (the record only; the gold is the caller's); whether it was, else why.
function tree.refund(rec: any, id: number): (boolean, string?)
	local ok, why = tree.canRefund(rec, id)
	if not ok then
		return false, why
	end
	local list = tree.of(rec).nodes
	table.remove(list, table.find(list, id) :: number)
	return true, nil
end

-- An act cleared for the first time at the character's tier: a point.
function tree.bossPoint(rec: any)
	tree.of(rec).bossPoints += 1
end

-- How many nodes of each spell and ability the class may have are taken (by name; by number; its
-- own region's and any other's it may use), and the creatures its summon nodes give.
local function counted(rec: any): ({ [string]: number }, { [number]: number }, { [string]: boolean })
	local class = classOf(rec)
	local spells: { [string]: number } = {}
	local abilities: { [number]: number } = {}
	local creatures: { [string]: boolean } = {}
	for id in tree.taken(rec) do
		local n = nodes[id]
		if n and tree.usable(class, n) then
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
-- healthRegen, manaRegen (a second), manaOnKill, spellCost, summonCost, summonHealth (%), own }: each node's
-- stats, the masteries of the spells and abilities the class has from them, and the stats' worth
-- of each spell, ability or summon node it can't use (data.crossWorth); own: the % more damage a
-- hit by a thing of each type deals (its spell's own bonus, data.rises), by type.
function tree.totals(rec: any): any
	local class = classOf(rec)
	local own: { [string]: number } = {}
	local sum = { health = 0, mana = 0, strength = 0, healthPct = 0, manaPct = 0, damage = 0, resist = 0, healthRegen = 0, manaRegen = 0,
		manaOnKill = 0, spellCost = 0, summonCost = 0, summonHealth = 0, own = own }
	local worth = (data.crossWorth :: any)[class] or {}
	for id in tree.taken(rec) do
		local n = nodes[id]
		if n then
			addStats(sum, n.stats, 1)
			if givesSkill(n) and not tree.usable(class, n) then
				addStats(sum, worth, 1)
			end
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
	{ "healthRegen", "+%.1f health a second" },
	{ "manaRegen", "+%.1f mana a second" },
	{ "manaOnKill", "+%d mana for each kill" },
	-- (a third word for a value below 0, said as its size)
	{ "spellCost", "spells cost %d%% more mana", "spells cost %d%% less mana" },
	{ "summonCost", "summonings cost %d%% more mana", "summonings cost %d%% less mana" },
	{ "summonHealth", "%+d%% most health for your summoned creatures" },
}

-- What a set of stats says ("+8 health, +3% damage dealt").
function tree.statText(stats: any): string
	local parts = {}
	for _, w in STAT_WORDS do
		local v = stats[w[1]]
		if v and v ~= 0 then
			if w[3] and v < 0 then
				table.insert(parts, string.format(w[3], -v))
			else
				table.insert(parts, string.format(w[2], v))
			end
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
		local only = " (the " .. CLASS_TITLES[n.region] .. "'s)"
		local r = data.rise(n.spell or n.ability)
		local mastery = tree.statText(r.mastery)
		-- (a level that makes its missile larger says so: data.rises' size)
		local larger = if r.size and r.size.byLevel[n.level] and not n.beyond
			then ": a larger, wider " .. title .. " (easier to hit with, a wider burst)"
			else ""
		if n.first then
			local own = if r.own ~= 0 then string.format(", and %+d%% %s damage", r.own, title) else ""
			local note = if r.note then ": " .. r.note else ""
			return title, "Unlocks " .. title .. " at level " .. n.level .. note .. larger .. own .. only
		elseif n.beyond then
			return title .. " mastery", mastery .. only
		elseif n.mastery and mastery ~= "" then
			return title .. " " .. n.level, title .. " to level " .. n.level .. larger .. ", and " .. mastery .. only
		end
		return title .. " " .. n.level, title .. " to level " .. n.level .. larger .. only
	elseif n.kind == "summon" then
		local title = tree.creatureTitle(n.creature)
		return "Summon " .. title, "The " .. title .. " in your field guide, to summon (the " .. CLASS_TITLES[n.region] .. "'s)"
	elseif n.kind == "root" then
		return n.name, "Where the " .. CLASS_TITLES[n.region] .. " starts (free for a " .. CLASS_TITLES[n.region] .. ")"
	end
	local kind = if n.kind == "keystone" then "Keystone: " elseif n.kind == "notable" then "Notable: " else ""
	return n.name, kind .. tree.statText(n.stats)
end

-- What a spell, ability or summon node gives a taker of each class's, as its tooltip says: a
-- class's stats' worth in words (data.crossWorth), by class.
function tree.worthText(class: string): string
	return tree.statText((data.crossWorth :: any)[class] or {})
end

-- Which classes may use a spell, ability or summon node's own gift (tree.usable): 1 a warrior, 2
-- a wizard, 4 a conjurer, summed; nil for any other node (it gives every class the same).
local CLASS_BITS: { { any } } = { { "warrior", 1 }, { "wizard", 2 }, { "conjurer", 4 } }
function tree.usableMask(n: any): number?
	if not givesSkill(n) then
		return nil
	end
	local mask = 0
	for _, c in CLASS_BITS do
		if tree.usable(c[1], n) then
			mask += c[2]
		end
	end
	return mask
end

-- The tooltip's line for a hero of `class` on a spell, ability or summon node (nil for any other
-- node): "For you: as above" (its own region's), "For you: as above (a Wizard can use it too)"
-- (another region's it may use), or its stats' worth in its place. The UI makes the same line
-- from the node's mask and the worth words (ui/main.luau's forYou).
function tree.forClass(n: any, class: string): string?
	if not givesSkill(n) then
		return nil
	end
	local title = CLASS_TITLES[class] or class
	if n.region == class then
		return "For you: as above"
	elseif tree.usable(class, n) then
		return "For you: as above (a " .. title .. " can use it too)"
	end
	return "For you: " .. tree.worthText(class) .. " in its place (a " .. title .. " can't use it)"
end

return tree
