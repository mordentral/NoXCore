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
	-- Act 2's. Their entries give `at` (the act's area index a type first shows at) in place of
	-- `from`, so they follow the act wherever its levels are tuned. No spellcaster in act 2 (by
	-- decision: they come from act 3 on, each one's spells checked as 18.3's were).
	-- The climb out: the old workings at the catacombs' mouth (act 2's first area, a Mines map: an
	-- area's `roster` in data/acts.luau), then the caves above them, where the beasts of the
	-- fields come in out of the weather.
	Workings = {
		{ type = "Bat", at = 1, weight = 3 },
		{ type = "SmallSpider", at = 1, weight = 2 },
		{ type = "Urchin", at = 1, weight = 4 },
		{ type = "GruntAxe", at = 1, weight = 3 },
		{ type = "Scorpion", at = 1, weight = 2 },
		{ type = "SpittingSpider", at = 1, weight = 1 },
	},
	Cave = {
		{ type = "Bat", at = 1, weight = 3 },
		{ type = "Spider", at = 1, weight = 3 },
		{ type = "Urchin", at = 1, weight = 3 },
		{ type = "GiantLeech", at = 1, weight = 2 },
		{ type = "Wolf", at = 2, weight = 3 },
		{ type = "AlbinoSpider", at = 2, weight = 1 },
		{ type = "Bear", at = 2, weight = 1 },
	},
	-- Above ground (the fields' first area is act 2's third: their `at` counts from the act's
	-- first area, the climb included).
	Grasslands = {
		{ type = "Wolf", at = 1, weight = 5 },
		{ type = "Urchin", at = 1, weight = 4 },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "Bear", at = 4, weight = 2 },
		{ type = "BlackWolf", at = 5, weight = 2 },
		{ type = "GruntAxe", at = 5, weight = 2 },
		{ type = "BlackBear", at = 7, weight = 1 },
		{ type = "OgreBrute", at = 7, weight = 1 },
	},
	Swamp = {
		{ type = "GiantLeech", at = 1, weight = 4 },
		{ type = "Spider", at = 1, weight = 3 },
		{ type = "Zombie", at = 1, weight = 3, rises = true },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "BlackWidow", at = 5, weight = 2 },
		{ type = "CarnivorousPlant", at = 5, weight = 1 },
		{ type = "Shade", at = 7, weight = 1 },
		{ type = "Troll", at = 7, weight = 1 },
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
-- (each for good); colour is its name plate's ({ r, g, b }: magic and rare as the loot of that
-- rarity is named; labels.luau sends it); drops the extra loot rolls its death makes. A monster
-- with a name (a `named` tier's) has its name over its head.
monsters.tiers = {
	normal = { health = 1.0, damage = 1.0, experience = 1.0, enchantments = 0, drops = 0, colour = { 225, 222, 210 } },
	magic = { health = 1.4, damage = 1.15, experience = 2.0, enchantments = 1, drops = 1, colour = { 110, 160, 255 } },
	rare = { health = 2.5, damage = 1.3, experience = 4.0, enchantments = 2, drops = 2, colour = { 250, 220, 80 }, named = true },
	champion = { health = 4.0, damage = 1.5, experience = 8.0, enchantments = 0, drops = 4, colour = { 255, 150, 40 }, named = true },
	-- (an act boss: its own enchantment set and drop, boss.luau)
	boss = { health = 6.0, damage = 1.4, experience = 20.0, enchantments = 0, drops = 0, colour = { 240, 70, 50 }, named = true },
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
	Workings = { name = "Rustjaw, the Tunnel Boss", type = "GruntAxe", enchantments = { "ENCHANT_SHIELD", "ENCHANT_HASTED" }, guards = 3 },
	Cave = { name = "Old Mother Den", type = "Bear", enchantments = { "ENCHANT_VAMPIRISM", "ENCHANT_SHIELD" }, guards = 3 },
}

-- The act bosses (data/acts.luau names each act's): its name, type, its own enchantment set (for
-- good), its guards at the start (magic), the adds it calls (each a type of its roster), what it
-- says, and its fight (boss.luau: "warlord", the default, or "barrow": barrowking.luau), whose
-- numbers are data/numbers.luau's (boss, barrowKing). `health`: its most health times this
-- (after the boss tier's; none: 1); `draw`: drawn this much larger on NoX Core clients.
monsters.bosses = {
	-- Act 1's: adds called at shares of its health, an enrage, a slam at a player near it.
	warlord = {
		name = "Gorvald, the Drowned Warlord",
		type = "OgreWarlord",
		fight = "warlord",
		enchantments = { "ENCHANT_SHIELD", "ENCHANT_PROTECT_FROM_MAGIC", "ENCHANT_VAMPIRISM" },
		enrage = "ENCHANT_HASTED",
		guards = 4,
		adds = { "Skeleton", "Zombie", "SkeletonLord" },
		lines = {
			summon = "Rise, drowned ones!",
			enrage = "Enough!",
		},
	},
	-- Act 2's: the barrow-king, a skeleton lord drawn large, in a ring of graves on his mound. His
	-- dead climb out of the graves round the arena (not at his side), he charges a player across it,
	-- then the grave-mist (the game's Toxic Cloud) under the party, and enraged the sky's fists (Fist
	-- of Vengeance) and a quake (barrowking.luau).
	barrowking = {
		name = "Vorthane, the Barrow-King",
		type = "SkeletonLord",
		fight = "barrow",
		health = 2.0,
		draw = 1.5,
		-- (his mist's and fists' hits: this share of what his own blows' scaling makes them)
		spells = 0.3,
		enchantments = { "ENCHANT_SHIELD", "ENCHANT_VAMPIRISM" },
		enrage = "ENCHANT_HASTED",
		guards = 2,
		-- (the guards at the start, and what the graves give up: the first list while he's above
		-- the mist's share of his health, the second below it)
		adds = { "Skeleton", "Zombie" },
		laterAdds = { "Skeleton", "Ghost", "VileZombie" },
		lines = {
			wake = "Who walks upon my barrow?",
			graves = "Rise, my oath-sworn!",
			charge = "Kneel!",
			mist = "Breathe the grave's breath.",
			enrage = "The barrow will not hold me!",
		},
	},
}

return monsters
