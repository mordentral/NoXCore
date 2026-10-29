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
--     and the conjurer a fourth, Beasts, of 12 nodes (column x = 9, south from y = 6): its
--     summons;
--     a branch's notables joined to the next branch's, so a region is a web, not three lines.
--
-- Node kinds: root (a class's start, free for that class), small (+a stat), notable (a bundle),
-- keystone (a trade: a big gain for a loss), spell (a wizard's or conjurer's spell), ability (a
-- warrior's ability), summon (a creature a conjurer can summon).
--
-- Spells come from the tree (by decision 2026-10-27). Each branch holds its own: the first node
-- of a spell along the branch unlocks it (at its start level, data.rises), each further
-- one raises it a level, up to 5; a node past what the spell's level is worth gives its mastery
-- (stats) too. A branch's 13 nodes in order: A small A B small notable A B small A B A keystone,
-- so spell A has five nodes and spell B three. A spell or ability node gives its spell and
-- mastery only to its region's class (another class may take it on its way through, for nothing
-- but the way).
--
-- Summons come from the tree too. The Beasts branch's 12 nodes in order: C small C C small
-- notable C C small C C keystone, each C the next of its creatures, weaker to stronger. A summon
-- node gives a conjurer the creature in its field guide and the summon spell that calls it.
--
-- Stats (as items' affixes are applied): health, mana, strength (flat), healthPct, manaPct (% of
-- the most), damage (% more dealt), resist (% less taken).
--
-- Node ids are their places in `nodes` and are kept in records: new nodes go at the end, never
-- between.
--
-- Pictures: a root, small, notable or keystone names its icon (`icon`, the mod's picture
-- assets\tree\<icon>.png, sent to the UI as "ic"): a root "root_<class>", a notable "n_<name>", a
-- keystone "k_<name>", a small by its stats ("s_health", "s_damage_resist"...). A new node of those
-- kinds names one that is there (a test checks every one). A spell, ability or summon node is drawn
-- with the game's own icon instead (`spellIcon`, sent to the UI as "si", drawn by nox.spellIcon):
-- a spell's its SPELL_* name, an ability's its ABILITY_* name (data.abilities' `icon`), a summon's
-- its creature's summon spell ("dd check" and a test prove each is a name the client knows).

local data = {}

-- The game's spells the tree gives, by name (their numbers from nox.spellId; "dd check" proves
-- each is the game's, its class's and in no linked group), and how the tree names them.
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

-- The creatures the tree's summon nodes give, by their field guide type (nox.grantCreature's and
-- nox.guideNumber's name): the summon spell that calls each, and how the tree names it. With the
-- book the mod's, a summoning wants both: the creature in the field guide and its summon spell in
-- the book, so a summon node gives both. Each is a creature of its own: none of the guide's
-- groups (the spiders come together), so a node gives one creature. "dd check" proves each, with
-- no player needed: its name the guide's, its spell the game's, a conjurer's summon and the one
-- that calls that guide entry.
data.creatures = {
	Bat = { spell = "SPELL_SUMMON_BAT", title = "Bat" },
	Wolf = { spell = "SPELL_SUMMON_WOLF", title = "Wolf" },
	BlackWolf = { spell = "SPELL_SUMMON_BLACK_WOLF", title = "Black Wolf" },
	Bear = { spell = "SPELL_SUMMON_BEAR", title = "Bear" },
	BlackBear = { spell = "SPELL_SUMMON_BLACK_BEAR", title = "Black Bear" },
	OgreWarlord = { spell = "SPELL_SUMMON_OGRE_WARLORD", title = "Ogre Warlord" },
	StoneGolem = { spell = "SPELL_SUMMON_STONE_GOLEM", title = "Stone Golem" },
}
-- The guide's first creature's summon spell: creature n of the guide (1-40) is called by the
-- spell n - 1 after it ("dd check" proves each creature's spell by it).
data.firstSummon = "SPELL_SUMMON_BAT"
data.guideSize = 40

-- A warrior's abilities by number (the game's: 1-5). An ability's level changes nothing in the
-- game once it is known (top 1, below), so each further node of one gives its mastery. `icon`:
-- the game's name for it, as nox.spellIcon takes it (the client's own icon).
data.abilities = {
	{ id = 1, key = "BerserkerCharge", title = "Berserker Charge", icon = "ABILITY_BERSERKER_CHARGE", mastery = { damage = 4 } },
	{ id = 2, key = "WarCry", title = "War Cry", icon = "ABILITY_WARCRY", mastery = { health = 10 } },
	{ id = 3, key = "Harpoon", title = "Harpoon", icon = "ABILITY_HARPOON", mastery = { damage = 3, strength = 2 } },
	{ id = 4, key = "TreadLightly", title = "Tread Lightly", icon = "ABILITY_TREAD_LIGHTLY", mastery = { resist = 3 } },
	{ id = 5, key = "EyeOfTheWolf", title = "Eye of the Wolf", icon = "ABILITY_EYE_OF_THE_WOLF", mastery = { strength = 4 } },
}

-- How the tree's spells rise with their nodes. The book is the mod's, so a spell is cast at the
-- level its nodes grant (not the network game's flat 3), and the game's own numbers set what a
-- level is worth (measured against a held foe; at levels 1-5, flat 3 being level 3):
--   Fireball      64 96 127 127 127 (its missile is a TitanFireball from level 3; 4 and 5 fly
--                 faster, and are larger: `size`, below)
--   Energy Bolt   38 48 60 68 77 a second
--   Magic Missile 16 24 32 40 48 (2-6 missiles of 8)
--   Pixie Swarm   16 24 32 40 48 (2-6 pixies of 8)
--   Fist          50 100 200 300 400
--   Meteor        45 65 85 105 125
--   Shock         15 30 45 60 75 a touch
--   Burn, Death Ray, Toxic Cloud, Lesser Heal, Slow: the same at every level.
-- Each spell's
--   start    the level its first node gives (1 when not named);
--   top      the highest level that adds anything worth a node (5 when not named);
--   mastery  what each of its nodes past `top` (or past 5) gives its class: still the spell's
--            level up to 5, and these stats;
--   own      % more damage the spell's own hits deal (those of the things in `hits`, its
--            missiles), given to its class by its first node and kept while the spell is;
--   size     its missile made larger at some levels: { missile = the type sized, byLevel =
--            { [level] = { hit, burst, draw } } }: a hero's cast at that level (its book's level)
--            gives each such missile it makes a hit circle of `hit` (nox.setHitRadius: a point
--            made a circle, so it meets what passes within `hit` of its path), its burst's
--            reach and push times `burst` (nox.setBurstScale), and its drawing times `draw`
--            (nox.setDrawScale: NoX Core clients at client level 5 draw the missile, and its
--            burst, that much larger; older and stock clients draw the stock ones). Walls still
--            meet its centre's path.
-- An ability rises as a spell with top 1 and its own mastery.
--
-- Fireball 4 and 5 (by decision 2026-10-28): larger and easier to hit with. A TitanFireball's
-- hit is its centre (a point) and its burst reaches 85 (a third of its 255), pushing to 170;
-- at 4 a hit circle of 8 and the burst x 1.25 (106), at 5 a circle of 16 and x 1.5 (127). The
-- masteries' +5% stay. It is drawn as large as its burst reaches: x 1.25 at 4, x 1.5 at 5.
--
-- Magic Missile and Pixie Swarm start at 1 with own +60%: a missile's or pixie's 8 is 13, so two
-- casts kill an area-1 Skeleton (50): Magic Missile for Fireball's mana (2 x 15 against 30), Pixie
-- Swarm in Meteor's two casts. +57% is the least that does (8 x 1.57 rounds to 13); a damage stat
-- that size for everything would beat any keystone, so it is the spell's alone.
data.rises = {
	SPELL_FIREBALL = {
		top = 3,
		mastery = { damage = 5 },
		size = {
			missile = "TitanFireball",
			byLevel = { [4] = { hit = 8, burst = 1.25, draw = 1.25 }, [5] = { hit = 16, burst = 1.5, draw = 1.5 } },
		},
	},
	SPELL_BURN = { top = 1, mastery = { damage = 4 } },
	SPELL_LIGHTNING = {},
	SPELL_SHOCK = {},
	SPELL_MAGIC_MISSILE = { own = 60, hits = { "MagicMissile" } },
	SPELL_DEATH_RAY = { top = 1, mastery = { damage = 4 } },
	SPELL_PIXIE_SWARM = { own = 60, hits = { "Pixie" } },
	SPELL_TOXIC_CLOUD = { top = 1, mastery = { damage = 4 } },
	SPELL_METEOR = {},
	SPELL_FIST = { start = 2 },
	SPELL_LESSER_HEAL = { top = 1, mastery = { health = 8 } },
	SPELL_SLOW = { top = 1, mastery = { resist = 3 } },
}

-- Each spell's (by name) and ability's (by number) rise, filled in, as an index.
local riseOf: { [any]: any } = {}
for name, r in (data.rises :: any) do
	riseOf[name] = {
		start = r.start or 1,
		top = math.min(r.top or 5, 5),
		mastery = r.mastery or {},
		own = r.own or 0,
		hits = r.hits or {},
		size = r.size,
	}
end
for _, a in data.abilities do
	riseOf[a.id] = { start = 1, top = 1, mastery = a.mastery, own = 0, hits = {} }
end
local PLAIN = { start = 1, top = 5, mastery = {}, own = 0, hits = {} }

-- A spell's (by name) or ability's (by number) rise: { start, top, mastery, own, hits, size? }.
function data.rise(give: string | number): any
	return riseOf[give] or PLAIN
end

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
local m1 = add({ x = 0, y = 0, kind = "notable", region = "middle", name = "Crossroads", icon = "n_crossroads", stats = { damage = 4, resist = 4 } })
local m2 = add({ x = -2, y = 0, kind = "small", region = "middle", name = "Vigour", icon = "s_health", stats = { health = 8 } })
local m3 = add({ x = 1, y = -2, kind = "small", region = "middle", name = "Insight", icon = "s_mana", stats = { mana = 8 } })
local m4 = add({ x = 1, y = 2, kind = "small", region = "middle", name = "Sinew", icon = "s_strength", stats = { strength = 3 } })
local m5 = add({ x = -2, y = -2, kind = "small", region = "middle", name = "Edge", icon = "s_damage", stats = { damage = 3 } })
local m6 = add({ x = -2, y = 2, kind = "small", region = "middle", name = "Hardiness", icon = "s_resist", stats = { resist = 3 } })
local m7 = add({ x = 2, y = 0, kind = "small", region = "middle", name = "Breath", icon = "s_health_mana", stats = { health = 6, mana = 6 } })
local m8 = add({ x = 4, y = 0, kind = "small", region = "middle", name = "Poise", icon = "s_damage_resist", stats = { damage = 2, resist = 2 } })
link(m1, m2)
link(m1, m3)
link(m1, m4)
link(m2, m5)
link(m2, m6)
link(m1, m7)
link(m7, m8)
link(m3, m7)
link(m4, m7)

-- A region: its root at (rx, ry) joined to the middle node `join`; its branches, each from
-- (x0, y0) stepping (dx, dy), joined to the root: a spell branch's 13 nodes in ORDER, a summon
-- branch's 12 in SUMMON_ORDER (its creatures in turn at the C's).
local ORDER = { "A", "small", "A", "B", "small", "notable", "A", "B", "small", "A", "B", "A", "keystone" }
local SUMMON_ORDER = { "C", "small", "C", "C", "small", "notable", "C", "C", "small", "C", "C", "keystone" }

local function region(class: string, title: string, rx: number, ry: number, join: number, branches: { any })
	local root = add({ x = rx, y = ry, kind = "root", region = class, name = title, icon = "root_" .. class, stats = {} })
	data.roots[class] = root
	link(join, root)
	local notables = {}
	for _, b in branches do
		local prev = root
		local smallAt = 0
		local creatureAt = 0
		local level: { [string]: number } = { A = 0, B = 0 }
		for k, what in if b.summons then SUMMON_ORDER else ORDER do
			local x, y = b.x0 + b.dx * (k - 1), b.y0 + b.dy * (k - 1)
			local n: any = { x = x, y = y, region = class, branch = b.name }
			local give = if what == "A" then b.A elseif what == "B" then b.B else nil
			if (what == "A" or what == "B") and give == nil then
				what = "small" -- (a branch without a second spell: a small stat there)
			end
			if what == "C" then
				creatureAt += 1
				n.kind = "summon"
				n.creature = b.summons[creatureAt]
				local c = (data.creatures :: any)[n.creature]
				n.spellIcon = c and c.spell
			elseif what == "A" or what == "B" then
				level[what] += 1
				if type(give) == "number" then
					n.kind = "ability"
					n.ability = give
					for _, a in data.abilities do
						if a.id == give then
							n.spellIcon = a.icon
						end
					end
				else
					n.kind = "spell"
					n.spell = give
					n.spellIcon = give
				end
				-- (as the node shows it, taken along its branch: the level it brings the spell to,
				-- whether it's the first, gives the mastery, or the mastery alone, past level 5)
				local r = data.rise(give)
				local raw = r.start + level[what] - 1
				n.level = math.min(5, raw)
				n.first = level[what] == 1
				n.mastery = raw > r.top
				n.beyond = raw > 5
			elseif what == "small" then
				smallAt += 1
				local s = b.small[(smallAt - 1) % #b.small + 1]
				n.kind = "small"
				n.name = s.name
				n.icon = s.icon
				n.stats = s.stats
			elseif what == "notable" then
				n.kind = "notable"
				n.name = b.notable.name
				n.icon = b.notable.icon
				n.stats = b.notable.stats
			else
				n.kind = "keystone"
				n.name = b.keystone.name
				n.icon = b.keystone.icon
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
	{ name = "Brawn", icon = "s_health", stats = { health = 8 } },
	{ name = "Might", icon = "s_strength", stats = { strength = 3 } },
	{ name = "Ferocity", icon = "s_damage", stats = { damage = 3 } },
	{ name = "Grit", icon = "s_resist", stats = { resist = 2 } },
}
local WIZARD_SMALL = {
	{ name = "Lore", icon = "s_mana", stats = { mana = 8 } },
	{ name = "Spark", icon = "s_damage", stats = { damage = 3 } },
	{ name = "Ward", icon = "s_health", stats = { health = 5 } },
}
local CONJURER_SMALL = {
	{ name = "Kinship", icon = "s_mana", stats = { mana = 6 } },
	{ name = "Bark", icon = "s_health", stats = { health = 6 } },
	{ name = "Thorn", icon = "s_damage", stats = { damage = 3 } },
	{ name = "Moss", icon = "s_resist", stats = { resist = 2 } },
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
		notable = { name = "Bloodlust", icon = "n_bloodlust", stats = { damage = 6, strength = 3 } },
		keystone = { name = "Bloodrage", icon = "k_bloodrage", stats = { damage = 25, resist = -10 } },
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
		notable = { name = "Ironhide", icon = "n_ironhide", stats = { resist = 5, health = 15 } },
		keystone = { name = "Unbowed", icon = "k_unbowed", stats = { resist = 15, healthPct = 15, damage = -10 } },
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
		notable = { name = "Keen Eye", icon = "n_keen_eye", stats = { damage = 5, health = 8 } },
		keystone = { name = "Predator", icon = "k_predator", stats = { damage = 15, strength = 10, healthPct = -10 } },
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
		notable = { name = "Kindling", icon = "n_kindling", stats = { damage = 6, mana = 8 } },
		keystone = { name = "Pyre Heart", icon = "k_pyre_heart", stats = { damage = 25, healthPct = -15 } },
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
		notable = { name = "Static Mind", icon = "n_static_mind", stats = { mana = 15, damage = 3 } },
		keystone = { name = "Overcharge", icon = "k_overcharge", stats = { manaPct = 25, damage = 10, resist = -10 } },
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
		notable = { name = "Focus", icon = "n_focus", stats = { mana = 10, resist = 3 } },
		keystone = { name = "Mind over Body", icon = "k_mind_over_body", stats = { manaPct = 40, healthPct = -20 } },
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
		notable = { name = "Brood", icon = "n_brood", stats = { damage = 5, mana = 8 } },
		keystone = { name = "Swarmcaller", icon = "k_swarmcaller", stats = { damage = 15, manaPct = 15, resist = -10 } },
	},
	{
		name = "Earth",
		x0 = 3,
		y0 = 6,
		dx = 0,
		dy = 1,
		-- (Meteor first, to level 5; Fist from level 2 to 4: Fist's 400 at level 5 was the tree's
		-- one outlier, and its 50 at level 1 a poor first spell)
		A = "SPELL_METEOR",
		B = "SPELL_FIST",
		small = { CONJURER_SMALL[4], CONJURER_SMALL[2] },
		notable = { name = "Stone Skin", icon = "n_stone_skin", stats = { resist = 5, health = 10 } },
		keystone = { name = "Rooted", icon = "k_rooted", stats = { resist = 20, damage = -10 } },
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
		notable = { name = "Gentle Hands", icon = "n_gentle_hands", stats = { health = 12, mana = 8 } },
		keystone = { name = "Lifebinder", icon = "k_lifebinder", stats = { healthPct = 25, damage = -15 } },
	},
	-- (added after the first three: its nodes' ids follow theirs, 129-140)
	{
		name = "Beasts",
		x0 = 9,
		y0 = 6,
		dx = 0,
		dy = 1,
		summons = { "Bat", "Wolf", "BlackWolf", "Bear", "BlackBear", "OgreWarlord", "StoneGolem" },
		small = { CONJURER_SMALL[1], CONJURER_SMALL[4] },
		notable = { name = "Pack Leader", icon = "n_pack_leader", stats = { mana = 12, health = 8 } },
		keystone = { name = "Beastmaster", icon = "k_beastmaster", stats = { manaPct = 20, resist = 10, damage = -15 } },
	},
})

return data
