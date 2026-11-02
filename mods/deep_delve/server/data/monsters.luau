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
	-- Act 3's (their `at` counts from act 3's first area, the wood's edge). Spellcasters come in
	-- from act 3 on (by decision), each type's spells checked first: none may cast Death or a
	-- Channel Life glyph (the two paths that skip the damage step, so the party rule couldn't
	-- cancel them). The engine gives a made creature its type's spells: the Urchin Shaman Inversion,
	-- Confuse, Pixie Swarm, Meteor and Push; the Will o' Wisp Chain Lightning. Both pass; nothing
	-- else here casts. (The Will o' Wisp is immune to fire and lightning: a wizard's Magic Missile
	-- or a blade is its end.)
	-- The wood's edge: the fields' beasts where the trees begin.
	Edge = {
		{ type = "Wolf", at = 1, weight = 4 },
		{ type = "Urchin", at = 1, weight = 3 },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "BlackWolf", at = 1, weight = 3 },
		{ type = "Bear", at = 1, weight = 2 },
		{ type = "GruntAxe", at = 1, weight = 1 },
	},
	-- The forest under the leaves: its wolves and bears, the urchins and their shamans, the plants
	-- that bite, then trolls and the wisps.
	Forest = {
		{ type = "Wolf", at = 1, weight = 4 },
		{ type = "BlackWolf", at = 1, weight = 3 },
		{ type = "Urchin", at = 1, weight = 3 },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "Bear", at = 2, weight = 2 },
		{ type = "WhiteWolf", at = 3, weight = 1 },
		{ type = "UrchinShaman", at = 3, weight = 1 },
		{ type = "CarnivorousPlant", at = 3, weight = 1 },
		{ type = "BlackBear", at = 5, weight = 2 },
		{ type = "Troll", at = 6, weight = 2 },
		{ type = "WillOWisp", at = 6, weight = 1 },
	},
	-- The mires in the wood's hollows: leeches, spiders and the plants that bite, wisps over the
	-- water, a troll now and then.
	Mire = {
		{ type = "GiantLeech", at = 1, weight = 3 },
		{ type = "Spider", at = 1, weight = 2 },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "BlackWidow", at = 1, weight = 2 },
		{ type = "CarnivorousPlant", at = 1, weight = 2 },
		{ type = "WillOWisp", at = 1, weight = 1 },
		{ type = "UrchinShaman", at = 1, weight = 1 },
		{ type = "Troll", at = 7, weight = 1 },
	},
	-- Act 4's (their `at` counts from act 4's first area, the dry steppe). Of the casters only the
	-- Urchin Shaman is here (its spells checked with act 3's: Inversion, Confuse, Pixie Swarm,
	-- Meteor, Push). The golems are the waste's heaviest (a stone golem's health is many a troll's):
	-- the mechanical ones from the act's middle, the stone ones rarer and later. The Mechanical Golem
	-- is immune to fire, the Stone Golem to lightning (thing.bin).
	-- The dry steppe: the wood's edge's beasts where the sand begins, and the waste's first.
	Steppe = {
		{ type = "Urchin", at = 1, weight = 3 },
		{ type = "Scorpion", at = 1, weight = 3 },
		{ type = "BlackWolf", at = 1, weight = 2 },
		{ type = "Wasp", at = 1, weight = 2 },
		{ type = "GruntAxe", at = 1, weight = 2 },
		{ type = "Imp", at = 1, weight = 1 },
	},
	-- The dunes and the ruins in them: scorpions and the old dead, imps and urchins, then the
	-- skeleton lords and the urchins' shamans, the ruins' golems last.
	Desert = {
		{ type = "Scorpion", at = 1, weight = 4 },
		{ type = "Skeleton", at = 1, weight = 3 },
		{ type = "Urchin", at = 1, weight = 3 },
		{ type = "Imp", at = 1, weight = 2 },
		{ type = "SkeletonLord", at = 3, weight = 2 },
		{ type = "UrchinShaman", at = 3, weight = 1 },
		{ type = "MechanicalGolem", at = 5, weight = 1 },
		{ type = "StoneGolem", at = 6, weight = 1 },
	},
	-- The bitter shore: urchins and their shamans on the sand, leeches in the shallows, scorpions,
	-- the drowned dead washed up; a skeleton lord now and then further on.
	Beach = {
		{ type = "Urchin", at = 1, weight = 4 },
		{ type = "Scorpion", at = 1, weight = 3 },
		{ type = "GiantLeech", at = 1, weight = 2 },
		{ type = "Skeleton", at = 1, weight = 2 },
		{ type = "UrchinShaman", at = 1, weight = 1 },
		{ type = "SpittingSpider", at = 1, weight = 1 },
		{ type = "SkeletonLord", at = 7, weight = 1 },
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
-- with a name (a `named` tier's) has its name over its head. powers: how many of the powers below
-- it rolls (by decision: a rare one, a champion two; magic and normal none; a boss none, its fight
-- its own).
monsters.tiers = {
	normal = { health = 1.0, damage = 1.0, experience = 1.0, enchantments = 0, drops = 0, colour = { 225, 222, 210 } },
	magic = { health = 1.4, damage = 1.15, experience = 2.0, enchantments = 1, drops = 1, colour = { 110, 160, 255 } },
	rare = { health = 2.5, damage = 1.3, experience = 4.0, enchantments = 2, drops = 2, colour = { 250, 220, 80 }, named = true, powers = 1 },
	champion = { health = 4.0, damage = 1.5, experience = 8.0, enchantments = 0, drops = 4, colour = { 255, 150, 40 }, named = true, powers = 2 },
	-- (an act boss: its own enchantment set and drop, boss.luau)
	boss = { health = 6.0, damage = 1.4, experience = 20.0, enchantments = 0, drops = 0, colour = { 240, 70, 50 }, named = true },
}

-- Rare and champion powers (powers.luau; by decision 2026-11-01). Each one a monster rolls puts its
-- word in front of the monster's name on its plate ("Frostbound Grimmaw", "Burning Swift Mother of
-- Graves"). In the order rolled from; each:
--   id        the console's name for it ("dd spawn rare 1 Skeleton burning,swift")
--   name      its word on the plate
--   notOn     the kinds of monster it never goes on (powerKinds below)
--   perLevel  how much stronger it is each area level past the first (its damage, its share, its
--             time; 0.03: 3% a level), on top of the monster's own scaling of the damage it deals
-- Damage a power deals is the monster's own (its tier's, its level's and the party's factors,
-- and the hero's resistance, as combat.luau scales any hit of its), and never touches the mod's
-- monsters (combat.luau's rule) or the party's own.
monsters.powers = {
	-- Its fire's motes circle it (the game's fire protection on it); every `beat` frames each foe
	-- within `reach` of it takes `damage` fire.
	{ id = "burning", name = "Burning", beat = 30, reach = 85, damage = 6, perLevel = 0.03, look = "ENCHANT_PROTECT_FROM_FIRE" },
	-- Each of its hits on a hero slows the hero (the game's Slow) for `slow` frames.
	{ id = "frostbound", name = "Frostbound", slow = 60, perLevel = 0.02 },
	-- Every `every` frames, a foe it sees further than `near` and within `far`: it steps through
	-- the air to the foe's side (`land` from it), the game's teleport flash where it left and arrived.
	{ id = "blinking", name = "Blinking", every = 150, near = 120, far = 320, land = 40, perLevel = 0, notOn = { "still" } },
	-- At its death it falls apart into `count` of its kind (normal), each with `health` of its most
	-- health and `experience` of its experience.
	{ id = "splitting", name = "Splitting", count = 2, health = 0.35, experience = 0.25, perLevel = 0, notOn = { "big", "still" } },
	-- Every `every` frames while a foe is within `wake`, the game's Force Field on the mod's other
	-- monsters within `reach` (at most `most`, at the spell's `power`), those without one already.
	{ id = "warding", name = "Warding", every = 210, reach = 180, most = 4, power = 2, perLevel = 0 },
	-- It heals `share` of the damage each of its hits on a hero does.
	{ id = "vampiric", name = "Vampiric", share = 0.4, perLevel = 0.02 },
	-- Hasted for good (the game's Haste).
	{ id = "swift", name = "Swift", enchant = "ENCHANT_HASTED", perLevel = 0, notOn = { "still" } },
	-- The game's reflective shield for good: missiles met at its front turn back on their shooter.
	{ id = "mirrored", name = "Mirrored", enchant = "ENCHANT_REFLECTIVE_SHIELD", perLevel = 0 },
	-- Every `every` frames while a foe is within `reach` of it, the game's Toxic Cloud (its own)
	-- where it stands.
	{ id = "festering", name = "Festering", every = 180, reach = 140, perLevel = 0 },
	-- At its death blue sparks gather where it fell; `warn` frames later lightning bursts there:
	-- `damage` (shock) to each hero within `reach`.
	{ id = "stormcharged", name = "Stormcharged", warn = 24, reach = 110, damage = 12, perLevel = 0.03 },
}

-- The kinds of monster some powers stay off (a power's `notOn`): big ones (none splits) and the
-- ones that never move (none blinks, splits or hastens).
monsters.powerKinds = {
	big = {
		Bear = true, BlackBear = true, Troll = true, OgreBrute = true, OgreWarlord = true, StoneGolem = true,
		MechanicalGolem = true, Demon = true, EmberDemon = true, Horrendous = true,
	},
	still = { CarnivorousPlant = true },
}

-- What a power adds to its monster's death: `drops` more loot (a part of one: better loot, loot.drop's
-- `better`), for each power it had: a harder monster's drop.
monsters.powerLoot = { drops = 0.5 }

-- How often a monster with a power that acts by itself (burning, blinking, warding, festering)
-- thinks it over (frames), and how near a foe must be for it to bother.
monsters.powerBeat = { every = 15, wake = 400 }

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
	-- (act 3's: the Forest's is the bear the expeditions' Forest runs end with too)
	Edge = { name = "Hedgefang, the Edge Stalker", type = "BlackWolf", enchantments = { "ENCHANT_HASTED", "ENCHANT_VAMPIRISM" }, guards = 4 },
	Forest = { name = "Old Bramblehide", type = "BlackBear", enchantments = { "ENCHANT_SHIELD", "ENCHANT_VAMPIRISM" }, guards = 3 },
	Mire = { name = "The Mire-Maw", type = "CarnivorousPlant", enchantments = { "ENCHANT_VAMPIRISM", "ENCHANT_PROTECT_FROM_MAGIC" }, guards = 4 },
	-- (act 4's: the Desert's and the Beach's end the expeditions' Desert and Beach runs too)
	Steppe = { name = "Kharrak, the Steppe Raider", type = "GruntAxe", enchantments = { "ENCHANT_HASTED", "ENCHANT_SHIELD" }, guards = 4 },
	Desert = { name = "Ossery, the Sand-Bound", type = "SkeletonLord", enchantments = { "ENCHANT_SHIELD", "ENCHANT_PROTECT_FROM_MAGIC" }, guards = 3 },
	Beach = { name = "Ulgra, the Shell-Witch", type = "UrchinShaman", enchantments = { "ENCHANT_SHIELD", "ENCHANT_VAMPIRISM" }, guards = 4 },
}

-- The act bosses (data/acts.luau names each act's): its name, type, its own enchantment set (for
-- good), its guards at the start (magic), the adds it calls (each a type of its roster), what it
-- says, and its fight (boss.luau: "warlord", the default; "barrow": barrowking.luau; "chief":
-- mosschief.luau; "warden": warden.luau), whose numbers are data/numbers.luau's (boss, barrowKing,
-- mossChief, ruinWarden). `health`: its most health times this
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
	-- Act 3's: the troll chieftain in his glade, drawn large. His wounds close as fast as they're
	-- made unless fire has touched him a moment ago (a fire spell, a burning blade, or his own fire
	-- pits at the glade's quarters); he hurls stones at whoever keeps away, and his kin run in from
	-- the glade's way in. Below the roots' share the wood's own Force of Nature rolls at the party
	-- and he hauls the last one who burnt him to his side; enraged, he stomps the ground round him
	-- and his stones come three at a time (mosschief.luau).
	mosschief = {
		name = "Brannog, the Moss-Chief",
		type = "Troll",
		fight = "chief",
		health = 2.0,
		draw = 1.4,
		-- (his Force of Nature's hits: this share of what his own blows' scaling makes them)
		spells = 0.3,
		enchantments = { "ENCHANT_SHIELD" },
		enrage = "ENCHANT_HASTED",
		guards = 2,
		-- (the guards at the start, and his kin from the way in: the first list above the roots'
		-- share of his health, the second below it)
		adds = { "Wolf", "BlackWolf" },
		laterAdds = { "BlackWolf", "Bear", "Troll" },
		lines = {
			wake = "Fresh meat for the fire-pits!",
			mend = "Cut me all you like. I mend.",
			burnt = "Rrargh! Fire!",
			kin = "Brothers! Hunt!",
			hurl = "Catch!",
			roots = "The wood is mine!",
			pull = "Come here, fire-bearer!",
			stomp = "Down!",
			enrage = "Now you see a troll's rage!",
		},
	},
	-- Act 4's: the ruin-warden, a stone golem drawn large, in the ruined court. Armoured in stone
	-- plates, he takes little of any blow; the ground heaves where he pounds, and each pound that
	-- strikes nobody shakes a plate off him and leaves him reeling and open. The court's statues and
	-- columns give up its wardens. Bare, he quakes the court (the game's Earthquake) and sets a
	-- sand-devil loose that slows and scours whoever it catches; enraged, the court's masonry falls
	-- round the party (warden.luau).
	warden = {
		name = "Udrahn, the Ruin-Warden",
		type = "StoneGolem",
		fight = "warden",
		-- (a stone golem's own health is many times a troll's: less than the others' factor)
		health = 0.5,
		draw = 1.5,
		-- (his quake's hits: this share of what his own blows' scaling makes them)
		spells = 0.3,
		enchantments = {},
		enrage = "ENCHANT_HASTED",
		guards = 2,
		-- (the guards at the start and the wardens the court gives up while he's plated, then the
		-- second list once he's bare)
		adds = { "FlyingGolem" },
		laterAdds = { "FlyingGolem", "SkeletonLord" },
		lines = {
			wake = "The court is closed to the living.",
			pound = "Be still!",
			plate = "Stone falls from stone...",
			wardens = "Wardens, wake!",
			bare = "No more stone to spare. Come, then!",
			sand = "The sands answer me!",
			masonry = "Let the court fall on you!",
			enrage = "I am the last wall standing!",
		},
	},
}

return monsters
