--!strict
-- Loot (design 4.3): what drops, how rare, its Deep Delve affixes and how it shows on the ground.
-- Every type is thing.bin's; the modifiers are modifier.bin's own, drawn at the drop by
-- nox.modifiers and nox.modifierFits (so only ones that fit the item), never named here.

local loot = {}

-- The gear that drops: { type, from (the lowest area level), weight }.
loot.gear = {
	-- weapons
	{ type = "Sword", from = 1, weight = 4 },
	{ type = "Longsword", from = 2, weight = 3 },
	{ type = "MorningStar", from = 2, weight = 2 },
	{ type = "BattleAxe", from = 3, weight = 2 },
	{ type = "WarHammer", from = 4, weight = 1 },
	{ type = "GreatSword", from = 5, weight = 1 },
	{ type = "Bow", from = 1, weight = 2 },
	{ type = "CrossBow", from = 3, weight = 1 },
	{ type = "StaffWooden", from = 1, weight = 2 },
	-- armour
	{ type = "LeatherBoots", from = 1, weight = 3 },
	{ type = "LeatherHelm", from = 1, weight = 3 },
	{ type = "LeatherArmor", from = 1, weight = 3 },
	{ type = "LeatherLeggings", from = 1, weight = 2 },
	{ type = "LeatherArmbands", from = 1, weight = 2 },
	{ type = "WizardRobe", from = 1, weight = 2 },
	{ type = "WizardHelm", from = 2, weight = 1 },
	{ type = "ConjurerHelm", from = 2, weight = 1 },
	{ type = "MedievalCloak", from = 1, weight = 2 },
	{ type = "WoodenShield", from = 1, weight = 2 },
	{ type = "ChainCoif", from = 2, weight = 2 },
	{ type = "ChainTunic", from = 3, weight = 2 },
	{ type = "ChainLeggings", from = 3, weight = 2 },
	{ type = "SteelHelm", from = 4, weight = 1 },
	{ type = "SteelShield", from = 4, weight = 1 },
	{ type = "PlateBoots", from = 5, weight = 1 },
	{ type = "Breastplate", from = 6, weight = 1 },
}

-- Potions: { type, weight }.
loot.potions = {
	{ type = "RedPotion", weight = 6 },
	{ type = "BluePotion", weight = 3 },
	{ type = "CurePoisonPotion", weight = 1 },
}

-- One loot roll: the chances (hundredths) that it gives gold, a potion, gear; else nothing.
loot.roll = { gold = 30, potion = 15, gear = 18 }

-- The rarity of a piece of gear, by weight; each area level adds `perLevel` to magic and rare.
-- (Unique items come with their data in Phase 6; legendaries, design 14.)
loot.rarity = {
	normal = 70,
	magic = 24,
	rare = 6,
	perLevel = { magic = 1.0, rare = 0.4 },
}

-- A modifier's worth (modifier.bin's WORTH) may be at most base + perLevel x area level; none past
-- `most` drop at all (the quest-only modifiers and the special materials stay out).
loot.worth = { base = 300, perLevel = 120, most = 2500 }

-- Deep Delve's own affixes (design 4.3), a rare's one or two: { id, text (%d its value), stat,
-- low, high (its value at area level 1), perLevel (more each area level) }. Applied while worn:
-- "damage" scales the damage its wearer deals (combat.luau), "health" and "mana" raise its most,
-- "manaRegen" adds to its mana a second (regen.luau; by decision 2026-10-31).
loot.affixes = {
	{ id = "damage", text = "+%d%% damage", stat = "damage", low = 4, high = 8, perLevel = 0.6 },
	{ id = "health", text = "+%d health", stat = "health", low = 4, high = 10, perLevel = 1.0 },
	{ id = "mana", text = "+%d mana", stat = "mana", low = 4, high = 10, perLevel = 1.0 },
	{ id = "manaRegen", text = "+%d mana a second", stat = "manaRegen", low = 1, high = 2, perLevel = 0.1 },
}

-- How each rarity shows on the ground (design 4.3): sparks in its colour (a point effect's
-- message number, nox.effect: 0x81 blue, 0x82 yellow, 0x84 violet) every `every` frames while it
-- lies there, and a sound as it lands. (The design's soft light under it waits for engine ask A9:
-- a light a script can make; see the design's 9.1.)
loot.ground = {
	normal = {},
	magic = { sparks = 0x81, every = 50 },
	rare = { sparks = 0x82, every = 30, sound = "Chime" },
	unique = { sparks = 0x84, every = 20, sound = "TripleChime" },
}
-- At most this many items on the ground spark at once (the oldest keep theirs; more wait).
loot.groundMost = 200

-- Its words: the rarity's name as the pickup's line says it.
loot.words = { normal = "", magic = "Magic", rare = "Rare", unique = "Unique" }

return loot
