--!strict
-- Loot (design 4.3): what drops, how rare, its Deep Delve affixes and how it shows on the ground.
-- Every type is thing.bin's; the modifiers are modifier.bin's own, drawn at the drop by
-- nox.modifiers and nox.modifierFits (so only ones that fit the item), never named here.

local loot = {}

-- The gear that drops: { type, from (the lowest area level), weight, kind (which of Deep Delve's
-- affixes it may carry: an affix's `kinds`; "weapon" a melee weapon, bow or crossbow, "staff" a
-- spell staff, "armour" worn armour and shields, "quiver" a quiver) }.
loot.gear = {
	-- weapons
	{ type = "Sword", from = 1, weight = 4, kind = "weapon" },
	{ type = "Longsword", from = 2, weight = 3, kind = "weapon" },
	{ type = "MorningStar", from = 2, weight = 2, kind = "weapon" },
	{ type = "BattleAxe", from = 3, weight = 2, kind = "weapon" },
	{ type = "WarHammer", from = 4, weight = 1, kind = "weapon" },
	{ type = "GreatSword", from = 5, weight = 1, kind = "weapon" },
	{ type = "Bow", from = 1, weight = 2, kind = "weapon" },
	{ type = "CrossBow", from = 3, weight = 1, kind = "weapon" },
	{ type = "StaffWooden", from = 1, weight = 2, kind = "weapon" },
	-- the arrows a bow or crossbow shoots (one quiver serves both; by decision 2026-11-01)
	{ type = "Quiver", from = 1, weight = 2, kind = "quiver" },
	-- the casters' staffs (each casts its own spell from its charges)
	{ type = "LesserFireballWand", from = 2, weight = 2, kind = "staff" },
	{ type = "SulphorousFlareWand", from = 3, weight = 2, kind = "staff" },
	{ type = "ForceWand", from = 4, weight = 2, kind = "staff" },
	{ type = "SulphorousShowerWand", from = 6, weight = 1, kind = "staff" },
	{ type = "FireStormWand", from = 8, weight = 1, kind = "staff" },
	{ type = "DeathRayWand", from = 12, weight = 1, kind = "staff" },
	{ type = "InfinitePainWand", from = 14, weight = 1, kind = "staff" },
	-- armour
	{ type = "LeatherBoots", from = 1, weight = 3, kind = "armour" },
	{ type = "LeatherHelm", from = 1, weight = 3, kind = "armour" },
	{ type = "LeatherArmor", from = 1, weight = 3, kind = "armour" },
	{ type = "LeatherLeggings", from = 1, weight = 2, kind = "armour" },
	{ type = "LeatherArmbands", from = 1, weight = 2, kind = "armour" },
	{ type = "WizardRobe", from = 1, weight = 2, kind = "armour" },
	{ type = "MedievalShirt", from = 1, weight = 2, kind = "armour" },
	{ type = "MedievalPants", from = 1, weight = 2, kind = "armour" },
	{ type = "WizardHelm", from = 2, weight = 1, kind = "armour" },
	{ type = "ConjurerHelm", from = 2, weight = 1, kind = "armour" },
	{ type = "MedievalCloak", from = 1, weight = 2, kind = "armour" },
	{ type = "WoodenShield", from = 1, weight = 2, kind = "armour" },
	{ type = "ChainCoif", from = 2, weight = 2, kind = "armour" },
	{ type = "ChainTunic", from = 3, weight = 2, kind = "armour" },
	{ type = "ChainLeggings", from = 3, weight = 2, kind = "armour" },
	{ type = "SteelHelm", from = 4, weight = 1, kind = "armour" },
	{ type = "SteelShield", from = 4, weight = 1, kind = "armour" },
	{ type = "PlateBoots", from = 5, weight = 1, kind = "armour" },
	{ type = "Breastplate", from = 6, weight = 1, kind = "armour" },
}

-- Potions: { type, weight }.
loot.potions = {
	{ type = "RedPotion", weight = 6 },
	{ type = "BluePotion", weight = 3 },
	{ type = "CurePoisonPotion", weight = 1 },
}

-- One loot roll: the chances (hundredths) that it gives gold, a potion, gear; else nothing.
loot.roll = { gold = 30, potion = 15, gear = 18 }

-- Breakable things (by decision 2026-11-11: every breakable in an area has a drop table): `kinds`
-- every thing.bin kind the game breaks into pieces (its death makes its broken pieces: barrels,
-- casks, crates, coffins, a wasps' nest; not the burning powder trail), each one of them in an area
-- watched from the area's set-up (breakables.luau). `drops` what each kind gives as it breaks, by
-- its type: the `chance` (0-1) of one small find, gold or a potion in the shares a loot roll gives
-- them (`roll` above), at the area level as a monster's are; `default` for a kind not named (one
-- the generator places later still gives). Gear stays the monsters' and the chests'. Nothing in
-- town gives anything.
loot.breakables = {
	kinds = {
		"Barrel", "Barrel2", "TargetBarrel1", "TargetBarrel2", "BarrelLOTD", "WaterBarrel",
		"LargeBarrel1", "LargeBarrel2", "PiledBarrels1", "PiledBarrels2", "PiledBarrels3", "PiledBarrels4",
		"BlackPowderBarrel", "BlackPowderBarrel2", "Crate1", "Crate2", "DarkCrate1", "DarkCrate2",
		"Coffin1", "Coffin2", "Coffin3", "Coffin4", "WaspNest",
	},
	drops = {
		-- (the generator's: the Dungeon's and the Mines' barrels, the Crypt's coffins; the towns'
		-- barrels and water barrels, which give nothing there)
		Barrel = { chance = 1 / 3 },
		Barrel2 = { chance = 1 / 3 },
		WaterBarrel = { chance = 1 / 3 },
		Coffin1 = { chance = 1 / 3 },
		Coffin2 = { chance = 1 / 3 },
		Coffin3 = { chance = 1 / 3 },
		Coffin4 = { chance = 1 / 3 },
		default = { chance = 1 / 3 },
	},
}

-- The rarity of a piece of gear, by weight; each area level adds `perLevel` to magic and rare.
-- (Uniques and set pieces are rolls of their own: data/uniques.luau.)
loot.rarity = {
	normal = 70,
	magic = 24,
	rare = 6,
	perLevel = { magic = 1.0, rare = 0.4 },
}

-- A modifier's worth (modifier.bin's WORTH) may be at most base + perLevel x area level; none past
-- `most` drop at all (the quest-only modifiers and the special materials stay out).
loot.worth = { base = 300, perLevel = 120, most = 2500 }

-- Only a modifier that does something drops (by decision 2026-11-01): one with an effect (its
-- nox.modifier(name).effects, by function) the engine runs. These are the effect functions the
-- game's items run (an engage's disengage comes with it); one not here (the game's Frost, Panic,
-- Resilience, Grip, Breaking, Puncture Prone, Parasite and Attraction functions, and a recharge's
-- blow) does nothing, and a modifier with no effect at all does nothing.
loot.effects = {
	DamageMultiplierEffect = true,
	StunEffect = true,
	FireEffect = true,
	FireRingEffect = true,
	BlueFireRingEffect = true,
	RecoilEffect = true,
	ConfuseEffect = true,
	LightningEffect = true,
	DrainManaEffect = true,
	VampirismEffect = true,
	PoisonEffect = true,
	SympathyEffect = true,
	ReadinessEffect = true,
	ProjectileSpeedEffect = true,
	ArmorMultiplierEffect = true,
	DurabilityMultiplierEffect = true,
	InversionEffect = true,
	RegenerationUpdate = true,
	ContinualReplenishmentUpdate = true,
	BrillianceEngage = true,
	SpeedEngage = true,
	FireProtectEngage = true,
	LightningProtectEngage = true,
	PoisonProtectEngage = true,
	RegenerationEngage = true,
}

-- The effects above that only scale one of the item's own numbers (nox.itemStats' field): on a
-- type without any of it the effect does nothing, so such a modifier drops only where it scales
-- something (loot.doesOn). The game's shields have no armour value (they block instead), so an
-- armour quality ("+20% armour") on one would do nothing.
loot.scales = {
	ArmorMultiplierEffect = "armor",
}

-- Deep Delve's own affixes (design 4.3), a rare's one or two: { id, text (%d its value), stat,
-- low, high (its value at area level 1), perLevel (more each area level), kinds (the gear kinds it
-- may roll on, loot.gear's `kind`; none: any), most (what all the worn pieces' together count
-- for at most), classes (none: any; else it rolls only on gear one of these classes may use, by
-- the game's record, and never on gear rolled for another class, as a vendor's stock is) }.
-- Applied while worn, each piece's value added to its wearer's stat:
--   damage      scales the damage its wearer deals (combat.luau);
--   health, mana raise its most;
--   manaRegen   adds to its mana a second (regen.luau; by decision 2026-10-31); never on a
--               warrior's own gear, nor on gear rolled for a warrior (by decision 2026-11-02).
-- The game's six modifiers that fit no item, as affixes (by decision 2026-11-01; affixes.luau):
--   frost       a weapon's: the chance (%) that its blow, or its arrow, chills one of the mod's
--               monsters (the game's slow, loot.hits.frost);
--   dread       a weapon's: the chance (%) that it frightens one (the game's fear: it flees);
--   grip        a weapon's: more damage (%) with its own blows and arrows (not spells, creatures
--               or its fire and shock);
--   reflect     armour's: the chance (%) that a monster's missile about to hit its wearer turns
--               back, the wearer's now (loot.reflect); a spell's thing with no missile (a boss's
--               mist or fist): the hit cancelled, its damage dealt to the monster;
--   ward        armour's: harmful enchantments on its wearer (loot.ward: its line calls them hexes)
--               last that much (%) less, and each poison step (loot.wardPoison);
--   resilience  armour's: its wearer's worn pieces wear that much (%) less (the durability each one
--               loses to a hit, given back in that share a frame on: affixes.wear).
-- A shield's own (by decision 2026-11-11; first-pass numbers): `slot` names the game slot it is
-- rolled in place of, on a type of `kinds` (a shield: data/tips.luau's kind) where no modifier of
-- that slot does anything (an armour quality on a shield: loot.scales); never one of a rare's
-- affixes besides:
--   guard       a shield's: blows and missiles from monsters in front of its wearer (loot.guard's
--               arc of its facing) deal that much (%) less (combat.luau).
-- `sheetMost`: the character sheet says its most after the total.
loot.affixes = {
	{ id = "damage", text = "+%d%% damage", stat = "damage", low = 4, high = 8, perLevel = 0.6 },
	{ id = "health", text = "+%d health", stat = "health", low = 4, high = 10, perLevel = 1.0 },
	{ id = "mana", text = "+%d mana", stat = "mana", low = 4, high = 10, perLevel = 1.0 },
	{ id = "manaRegen", text = "+%d mana a second", stat = "manaRegen", low = 1, high = 2, perLevel = 0.1, classes = { wizard = true, conjurer = true } },
	{ id = "frost", text = "Frost: %d%% chance to chill on a hit", stat = "frost", low = 8, high = 14, perLevel = 0.5, kinds = { weapon = true }, most = 50 },
	{ id = "dread", text = "Dread: %d%% chance to frighten on a hit", stat = "dread", low = 4, high = 8, perLevel = 0.3, kinds = { weapon = true }, most = 30 },
	{ id = "grip", text = "Grip: +%d%% damage with this weapon", stat = "grip", low = 5, high = 10, perLevel = 0.5, kinds = { weapon = true } },
	{ id = "reflect", text = "Reflection: %d%% to turn missiles back", stat = "reflect", low = 4, high = 8, perLevel = 0.3, kinds = { armour = true }, most = 35 },
	{ id = "ward", text = "Spell Protection: hexes %d%% shorter", stat = "ward", low = 10, high = 18, perLevel = 0.8, kinds = { armour = true }, most = 75 },
	-- (its most 80 by decision 2026-11-11, from 60; shown on the sheet)
	{ id = "resilience", text = "Resilience: worn gear wears %d%% less", stat = "resilience", low = 10, high = 18, perLevel = 0.8, kinds = { armour = true }, most = 80, sheetMost = true },
	{ id = "guard", text = "Guard: %d%% less from blows and missiles ahead", stat = "guard", low = 8, high = 12, perLevel = 0.3, kinds = { shield = true }, slot = "effectiveness", most = 40 },
}

-- The guard affix's front: within `arc` degrees either side of its wearer's facing (about the
-- front a raised shield blocks), from the monster behind the hit; the hits it lessens, by cause.
loot.guard = { arc = 67.5, causes = { blow = true, missile = true } }

-- What a frost or dread affix puts on a monster it procs on: the game's enchantment, for how many
-- frames, at what power; `spare`: monster tiers it never takes (a boss doesn't flee). Never one
-- shorter than what the monster has of it already. (A fear-proof creature refuses fear outside a
-- solo game, as the game has it.)
loot.hits = {
	frost = { enchantment = "ENCHANT_SLOWED", frames = 60, power = 1 },
	dread = { enchantment = "ENCHANT_AFRAID", frames = 45, power = 1, spare = { boss = true } },
}

-- A monster's missile a reflect affix turned: its hits dealt as its shooter's would have been on
-- the wearer (its tier and area level, a boss's `spells`), not by the wearer's bonuses, for `keep`
-- frames after the turn.
loot.reflect = { keep = 30 * 10 }

-- The harmful enchantments a ward affix shortens on its wearer: those the game's monsters and
-- traps put on a player that run out by themselves.
loot.ward = { "ENCHANT_SLOWED", "ENCHANT_CONFUSED", "ENCHANT_HELD", "ENCHANT_BLINDED", "ENCHANT_ANTI_MAGIC" }

-- The share of a ward affix's % a poison step is cut by (1: all of it): each step, the frames till
-- the poison falls a level, that much shorter; its level, and so its bites, as the game's.
loot.wardPoison = 1

-- How each rarity shows on the ground (design 4.3): sparks in its colour (a point effect's
-- message number, nox.effect: 0x81 blue, 0x82 yellow, 0x83 cyan, 0x84 violet) every `every` frames while it
-- lies there, and a sound as it lands. (The design's soft light under it waits for engine ask A9:
-- a light a script can make; see the design's 9.1.)
loot.ground = {
	normal = {},
	magic = { sparks = 0x81, every = 50 },
	rare = { sparks = 0x82, every = 30, sound = "Chime" },
	unique = { sparks = 0x84, every = 20, sound = "TripleChime" },
	set = { sparks = 0x83, every = 20, sound = "TripleChime" },
}
-- At most this many items on the ground spark at once (the oldest keep theirs; more wait).
loot.groundMost = 200

-- Its words: the rarity's name as the pickup's line says it.
loot.words = { normal = "", magic = "Magic", rare = "Rare", unique = "Unique", set = "Set" }

return loot
