--!strict
-- Monsters (design 3.3): the rosters each biome draws its packs from, the tiers, and the named
-- champions. Every type is thing.bin's (console "dd check" tests them all).

local monsters = {}

-- Each biome's roster: { type, from (the lowest area level it shows at) or at (the lowest area
-- index of its act), weight, rises (true: it gets up again some time after its death, as the
-- game's zombies do), light (true: a light monster, the kind the opening's first stretch is led
-- by, numbers.areas.opening's `light`) }.
monsters.rosters = {
	Crypt = {
		{ type = "Zombie", from = 1, weight = 5, rises = true },
		{ type = "Skeleton", from = 1, weight = 4 },
		{ type = "Bat", from = 1, weight = 2, light = true },
		{ type = "Rat", from = 1, weight = 2, light = true },
		{ type = "Ghost", from = 2, weight = 2 },
		{ type = "VileZombie", from = 3, weight = 2, rises = true },
		{ type = "SkeletonLord", from = 4, weight = 1 },
	},
	Dungeon = {
		{ type = "Rat", from = 1, weight = 4, light = true },
		{ type = "GiantLeech", from = 1, weight = 3 },
		{ type = "SmallSpider", from = 1, weight = 3, light = true },
		{ type = "Skeleton", from = 2, weight = 3 },
		{ type = "Spider", from = 3, weight = 2 },
		{ type = "Imp", from = 3, weight = 2 },
	},
	Mines = {
		{ type = "Bat", from = 1, weight = 3, light = true },
		{ type = "SmallSpider", from = 1, weight = 3, light = true },
		{ type = "Urchin", from = 1, weight = 3 },
		{ type = "Scorpion", from = 3, weight = 2 },
		{ type = "GruntAxe", from = 3, weight = 2 },
		{ type = "SpittingSpider", from = 4, weight = 1 },
	},
	-- Act 2's biomes, above ground. Their entries give `at` (the act's area index a type first
	-- shows at) in place of `from`, so they follow the act wherever its levels are tuned. No
	-- spellcaster yet (the party rules' check of what can reach a player stands as it was).
	Grasslands = {
		{ type = "Wolf", at = 1, weight = 5 },
		{ type = "Urchin", at = 1, weight = 4 },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "Bear", at = 2, weight = 2 },
		{ type = "BlackWolf", at = 3, weight = 2 },
		{ type = "GruntAxe", at = 3, weight = 2 },
		{ type = "BlackBear", at = 5, weight = 1 },
		{ type = "OgreBrute", at = 5, weight = 1 },
	},
	Swamp = {
		{ type = "GiantLeech", at = 1, weight = 4 },
		{ type = "Spider", at = 1, weight = 3 },
		{ type = "Zombie", at = 1, weight = 3, rises = true },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "BlackWidow", at = 3, weight = 2 },
		{ type = "CarnivorousPlant", at = 3, weight = 1 },
		{ type = "Shade", at = 5, weight = 1 },
		{ type = "Troll", at = 5, weight = 1 },
	},
}

-- Monsters that rise again (the rosters' `rises`) are kept away from where players come in (by
-- decision 2026-10-29: one getting up again is too hard that early):
--   arrivalReach  never in a pack whose spot lies within this of a room players arrive in (an
--                 area's start, a waypoint's arrival; a sum of the x and y distances), in any area
--   clearUntil    by area level: never in a pack within this share of the route from its start,
--                 so they come in over act 1 (the first area's first rooms have none)
monsters.rising = {
	arrivalReach = 23 * 16,
	clearUntil = { [1] = 0.6, [2] = 0.4, [3] = 0.2 },
}

-- The tiers, in order (design 3.3). health and damage multiply the monster's own (after the area
-- level's); experience its experience; enchantments is how many of the tier's list it gets
-- (each for good); colour is its name's (for A7's name plate; a text line's colour today is the
-- client's own); drops the extra loot rolls its death makes.
monsters.tiers = {
	normal = { health = 1.0, damage = 1.0, experience = 1.0, enchantments = 0, drops = 0, colour = "white" },
	magic = { health = 1.4, damage = 1.15, experience = 2.0, enchantments = 1, drops = 1, colour = "blue" },
	rare = { health = 2.5, damage = 1.3, experience = 4.0, enchantments = 2, drops = 2, colour = "gold", named = true },
	champion = { health = 4.0, damage = 1.5, experience = 8.0, enchantments = 0, drops = 4, colour = "orange", named = true },
	-- (an act boss: its own enchantment set and drop, boss.luau)
	boss = { health = 6.0, damage = 1.4, experience = 20.0, enchantments = 0, drops = 0, colour = "red", named = true },
}

-- The chances of a pack's tier (its leader's; its followers are normal, or magic with a rare one),
-- in hundredths: magic and rare, the rest normal. Each area level adds `perLevel` to each.
monsters.packTiers = { magic = 18, rare = 5, perLevel = { magic = 1, rare = 0.5 } }

-- The enchantments magic and rare monsters draw from (nox.enchant's names).
monsters.enchantments = { "ENCHANT_HASTED", "ENCHANT_VAMPIRISM", "ENCHANT_SHIELD", "ENCHANT_PROTECT_FROM_MAGIC" }

-- A rare's name: one of each list.
monsters.rareNames = {
	first = { "Grim", "Rot", "Ash", "Bone", "Gloom", "Dread", "Hollow", "Murk", "Pale", "Sour" },
	second = { "maw", "fang", "shade", "gut", "claw", "eye", "spine", "wail", "husk", "brand" },
}

-- Champions: a named rare with a fixed set (design 3.3; side areas from Phase 2). Phase 1 puts
-- one in each area's end chamber, its biome's.
monsters.champions = {
	Crypt = { name = "Mother of Graves", type = "VileZombie", enchantments = { "ENCHANT_VAMPIRISM", "ENCHANT_SHIELD" }, guards = 3 },
	Dungeon = { name = "The Gnawing King", type = "GiantLeech", enchantments = { "ENCHANT_HASTED", "ENCHANT_VAMPIRISM" }, guards = 3 },
	Mines = { name = "Old Stinger", type = "Scorpion", enchantments = { "ENCHANT_HASTED", "ENCHANT_SHIELD" }, guards = 3 },
	Grasslands = { name = "Greymuzzle, the Pack Sire", type = "WhiteWolf", enchantments = { "ENCHANT_HASTED", "ENCHANT_VAMPIRISM" }, guards = 4 },
	Swamp = { name = "The Bog Widow", type = "BlackWidow", enchantments = { "ENCHANT_SHIELD", "ENCHANT_VAMPIRISM" }, guards = 3 },
}

-- The act bosses (data/acts.luau names each act's): its name, type, its own enchantment set (for
-- good), the one it takes when enraged, its guards at the start (magic), the adds it calls (each
-- a type of its roster, magic), and what it says.
monsters.bosses = {
	warlord = {
		name = "Gorvald, the Drowned Warlord",
		type = "OgreWarlord",
		enchantments = { "ENCHANT_SHIELD", "ENCHANT_PROTECT_FROM_MAGIC", "ENCHANT_VAMPIRISM" },
		enrage = "ENCHANT_HASTED",
		guards = 4,
		adds = { "Skeleton", "Zombie", "SkeletonLord" },
		lines = {
			summon = "Rise, drowned ones!",
			enrage = "Enough!",
		},
	},
	-- Act 2's boss. A placeholder: a strong champion on act 1's fight (the same phases, slam and
	-- numbers), its own name, type and adds, until act 2 has a fight of its own.
	hillbreaker = {
		name = "Brakka the Hillbreaker",
		type = "OgreBrute",
		enchantments = { "ENCHANT_SHIELD", "ENCHANT_VAMPIRISM" },
		enrage = "ENCHANT_HASTED",
		guards = 4,
		adds = { "GruntAxe", "Wolf", "Urchin" },
		lines = {
			summon = "To me, brothers!",
			enrage = "Brakka breaks you!",
		},
	},
}

return monsters
