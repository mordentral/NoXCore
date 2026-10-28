--!strict
-- The passive tree (design 4.5): about 120 nodes in three class regions joined in the middle.
--
-- Its shape, on a grid (one unit about 30 pixels in the tree window; x east, y south):
--
--   * the middle (8 nodes) round (0, 0), joining the three regions;
--   * each class region a root and three branches of 13 nodes:
--       the warrior's to the west (rows y = -3, 0, 3, running west from x = -6),
--       the wizard's to the north (columns x = 0, 3, 6, running north from y = -6),
--       the conjurer's to the south (columns x = 0, 3, 6, running south from y = 6);
--     a branch's notables joined to the next branch's, so a region is a web, not three lines.
--
-- Node kinds: root (a class's start, free for that class), small (+a stat), notable (a bundle),
-- keystone (a trade: a big gain for a loss), spell (a wizard's or conjurer's spell), ability (a
-- warrior's ability).
--
-- Spells come from the tree (by decision 2026-10-27). Each branch holds its own: the first node
-- of a spell along the branch unlocks it at level 1, each further one raises it a level, up to 5.
-- A branch's 13 nodes in order: A small A B small notable A B small A B A keystone, so spell A
-- has five nodes and spell B three. A spell or ability node gives its spell only to its region's
-- class (another class may take it on its way through, for nothing but the way).
--
-- Stats (as items' affixes are applied): health, mana, strength (flat), healthPct, manaPct (% of
-- the most), damage (% more dealt), resist (% less taken).
--
-- Node ids are their places in `nodes` and are kept in records: new nodes go at the end, never
-- between.

local data = {}

-- The game's spells the tree gives, by name with their numbers (the game's spell table; "dd
-- check" proves every pair, and that each is its class's and in no linked group).
data.spells = {
	SPELL_FIREBALL = 27,
	SPELL_BURN = 5,
	SPELL_LIGHTNING = 24,
	SPELL_SHOCK = 71,
	SPELL_MAGIC_MISSILE = 50,
	SPELL_DEATH_RAY = 16,
	SPELL_PIXIE_SWARM = 58,
	SPELL_TOXIC_CLOUD = 128,
	SPELL_FIST = 29,
	SPELL_METEOR = 52,
	SPELL_LESSER_HEAL = 41,
	SPELL_SLOW = 72,
}

-- How the tree names them.
data.spellTitles = {
	SPELL_FIREBALL = "Fireball",
	SPELL_BURN = "Burn",
	SPELL_LIGHTNING = "Energy Bolt",
	SPELL_SHOCK = "Shock",
	SPELL_MAGIC_MISSILE = "Magic Missile",
	SPELL_DEATH_RAY = "Death Ray",
	SPELL_PIXIE_SWARM = "Pixie Swarm",
	SPELL_TOXIC_CLOUD = "Toxic Cloud",
	SPELL_FIST = "Fist of Vengeance",
	SPELL_METEOR = "Meteor",
	SPELL_LESSER_HEAL = "Lesser Heal",
	SPELL_SLOW = "Slow",
}

-- A warrior's abilities by number (the game's: 1-5).
data.abilities = {
	{ id = 1, key = "BerserkerCharge", title = "Berserker Charge" },
	{ id = 2, key = "WarCry", title = "War Cry" },
	{ id = 3, key = "Harpoon", title = "Harpoon" },
	{ id = 4, key = "TreadLightly", title = "Tread Lightly" },
	{ id = 5, key = "EyeOfTheWolf", title = "Eye of the Wolf" },
}

-- The class flags a spell of each class carries (thing.bin's: 0x1000000 any caster).
data.classFlags = { wizard = 0x2000000, conjurer = 0x4000000 }
data.anyCaster = 0x1000000
-- Spell flags of the game's linked groups (a spell in one moves its group with it): none of the
-- tree's may carry these.
data.groupFlags = 0x1000 + 0x2000 + 0x4000 + 0x8000 + 0x10000 + 0x20000

-- A respec at the town's fountain: base + perNode x the nodes taken, in gold.
data.respec = { base = 50, perNode = 15 }
-- Resistance is held within these (% less taken).
data.resist = { least = -50, most = 75 }

-- ---- The nodes

local nodes: { any } = {}
data.nodes = nodes
-- Each class's root node id.
data.roots = {} :: { [string]: number }

local function add(n: any): number
	n.id = #nodes + 1
	n.links = n.links or {}
	n.stats = n.stats or {}
	table.insert(nodes, n)
	return n.id
end

local function link(a: number, b: number)
	table.insert(nodes[a].links, b)
end

-- The middle: a crossroads and the ways out to the three roots.
local m1 = add({ x = 0, y = 0, kind = "notable", region = "middle", name = "Crossroads", stats = { damage = 4, resist = 4 } })
local m2 = add({ x = -2, y = 0, kind = "small", region = "middle", name = "Vigour", stats = { health = 8 } })
local m3 = add({ x = 1, y = -2, kind = "small", region = "middle", name = "Insight", stats = { mana = 8 } })
local m4 = add({ x = 1, y = 2, kind = "small", region = "middle", name = "Sinew", stats = { strength = 3 } })
local m5 = add({ x = -2, y = -2, kind = "small", region = "middle", name = "Edge", stats = { damage = 3 } })
local m6 = add({ x = -2, y = 2, kind = "small", region = "middle", name = "Hardiness", stats = { resist = 3 } })
local m7 = add({ x = 2, y = 0, kind = "small", region = "middle", name = "Breath", stats = { health = 6, mana = 6 } })
local m8 = add({ x = 4, y = 0, kind = "small", region = "middle", name = "Poise", stats = { damage = 2, resist = 2 } })
link(m1, m2)
link(m1, m3)
link(m1, m4)
link(m2, m5)
link(m2, m6)
link(m1, m7)
link(m7, m8)
link(m3, m7)
link(m4, m7)

-- A region: its root at (rx, ry) joined to the middle node `join`; its branches, each 13 nodes
-- from (x0, y0) stepping (dx, dy), joined to the root.
local ORDER = { "A", "small", "A", "B", "small", "notable", "A", "B", "small", "A", "B", "A", "keystone" }

local function region(class: string, title: string, rx: number, ry: number, join: number, branches: { any })
	local root = add({ x = rx, y = ry, kind = "root", region = class, name = title, stats = {} })
	data.roots[class] = root
	link(join, root)
	local notables = {}
	for _, b in branches do
		local prev = root
		local smallAt = 0
		local level: { [string]: number } = { A = 0, B = 0 }
		for k, what in ORDER do
			local x, y = b.x0 + b.dx * (k - 1), b.y0 + b.dy * (k - 1)
			local n: any = { x = x, y = y, region = class, branch = b.name }
			local give = if what == "A" then b.A elseif what == "B" then b.B else nil
			if (what == "A" or what == "B") and give == nil then
				what = "small" -- (a branch without a second spell: a small stat there)
			end
			if what == "A" or what == "B" then
				level[what] += 1
				if type(give) == "number" then
					n.kind = "ability"
					n.ability = give
				else
					n.kind = "spell"
					n.spell = give
				end
				n.level = level[what]
			elseif what == "small" then
				smallAt += 1
				local s = b.small[(smallAt - 1) % #b.small + 1]
				n.kind = "small"
				n.name = s.name
				n.stats = s.stats
			elseif what == "notable" then
				n.kind = "notable"
				n.name = b.notable.name
				n.stats = b.notable.stats
			else
				n.kind = "keystone"
				n.name = b.keystone.name
				n.stats = b.keystone.stats
			end
			local id = add(n)
			link(prev, id)
			prev = id
			if what == "notable" then
				table.insert(notables, id)
			end
		end
	end
	-- (each branch's notable joined to the next's)
	for i = 1, #notables - 1 do
		link(notables[i], notables[i + 1])
	end
end

local WARRIOR_SMALL = {
	{ name = "Brawn", stats = { health = 8 } },
	{ name = "Might", stats = { strength = 3 } },
	{ name = "Ferocity", stats = { damage = 3 } },
	{ name = "Grit", stats = { resist = 2 } },
}
local WIZARD_SMALL = {
	{ name = "Lore", stats = { mana = 8 } },
	{ name = "Spark", stats = { damage = 3 } },
	{ name = "Ward", stats = { health = 5 } },
}
local CONJURER_SMALL = {
	{ name = "Kinship", stats = { mana = 6 } },
	{ name = "Bark", stats = { health = 6 } },
	{ name = "Thorn", stats = { damage = 3 } },
	{ name = "Moss", stats = { resist = 2 } },
}

region("warrior", "The Warrior's Path", -4, 0, m2, {
	{
		name = "Fury",
		x0 = -6,
		y0 = -3,
		dx = -1,
		dy = 0,
		A = 1, -- Berserker Charge
		B = 2, -- War Cry
		small = WARRIOR_SMALL,
		notable = { name = "Bloodlust", stats = { damage = 6, strength = 3 } },
		keystone = { name = "Bloodrage", stats = { damage = 25, resist = -10 } },
	},
	{
		name = "Guard",
		x0 = -6,
		y0 = 0,
		dx = -1,
		dy = 0,
		A = 4, -- Tread Lightly
		B = nil,
		small = { WARRIOR_SMALL[4], WARRIOR_SMALL[1] },
		notable = { name = "Ironhide", stats = { resist = 5, health = 15 } },
		keystone = { name = "Unbowed", stats = { resist = 15, healthPct = 15, damage = -10 } },
	},
	{
		name = "Hunt",
		x0 = -6,
		y0 = 3,
		dx = -1,
		dy = 0,
		A = 3, -- Harpoon
		B = 5, -- Eye of the Wolf
		small = { WARRIOR_SMALL[3], WARRIOR_SMALL[2] },
		notable = { name = "Keen Eye", stats = { damage = 5, health = 8 } },
		keystone = { name = "Predator", stats = { damage = 15, strength = 10, healthPct = -10 } },
	},
})

region("wizard", "The Wizard's Path", 3, -4, m3, {
	{
		name = "Pyromancy",
		x0 = 0,
		y0 = -6,
		dx = 0,
		dy = -1,
		A = "SPELL_FIREBALL",
		B = "SPELL_BURN",
		small = WIZARD_SMALL,
		notable = { name = "Kindling", stats = { damage = 6, mana = 8 } },
		keystone = { name = "Pyre Heart", stats = { damage = 25, healthPct = -15 } },
	},
	{
		name = "Storm",
		x0 = 3,
		y0 = -6,
		dx = 0,
		dy = -1,
		A = "SPELL_LIGHTNING",
		B = "SPELL_SHOCK",
		small = { WIZARD_SMALL[2], WIZARD_SMALL[1] },
		notable = { name = "Static Mind", stats = { mana = 15, damage = 3 } },
		keystone = { name = "Overcharge", stats = { manaPct = 25, damage = 10, resist = -10 } },
	},
	{
		name = "Arcana",
		x0 = 6,
		y0 = -6,
		dx = 0,
		dy = -1,
		A = "SPELL_MAGIC_MISSILE",
		B = "SPELL_DEATH_RAY",
		small = { WIZARD_SMALL[3], WIZARD_SMALL[1] },
		notable = { name = "Focus", stats = { mana = 10, resist = 3 } },
		keystone = { name = "Mind over Body", stats = { manaPct = 40, healthPct = -20 } },
	},
})

region("conjurer", "The Conjurer's Path", 3, 4, m4, {
	{
		name = "Wilds",
		x0 = 0,
		y0 = 6,
		dx = 0,
		dy = 1,
		A = "SPELL_PIXIE_SWARM",
		B = "SPELL_TOXIC_CLOUD",
		small = { CONJURER_SMALL[3], CONJURER_SMALL[1] },
		notable = { name = "Brood", stats = { damage = 5, mana = 8 } },
		keystone = { name = "Swarmcaller", stats = { damage = 15, manaPct = 15, resist = -10 } },
	},
	{
		name = "Earth",
		x0 = 3,
		y0 = 6,
		dx = 0,
		dy = 1,
		A = "SPELL_FIST",
		B = "SPELL_METEOR",
		small = { CONJURER_SMALL[4], CONJURER_SMALL[2] },
		notable = { name = "Stone Skin", stats = { resist = 5, health = 10 } },
		keystone = { name = "Rooted", stats = { resist = 20, damage = -10 } },
	},
	{
		name = "Grace",
		x0 = 6,
		y0 = 6,
		dx = 0,
		dy = 1,
		A = "SPELL_LESSER_HEAL",
		B = "SPELL_SLOW",
		small = { CONJURER_SMALL[2], CONJURER_SMALL[1] },
		notable = { name = "Gentle Hands", stats = { health = 12, mana = 8 } },
		keystone = { name = "Lifebinder", stats = { healthPct = 25, damage = -15 } },
	},
})

return data
